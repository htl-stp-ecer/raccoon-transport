#include "raccoon/ChannelTable.h"
#include "raccoon/Channels.h"
#include "raccoon/Options.h"
#include "raccoon/Transport.h"
#include "raccoon/raccoon_ring.h"

#include <raccoon/scalar_i32_t.hpp>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    struct TestFailure : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw TestFailure(message);
        }
    }

    void resetRing(const std::string& channel)
    {
        char path[512];
        require(rrb_channel_to_path(channel.c_str(), path, sizeof(path)) == 0, "path of " + channel);
        (void)unlink(path);
    }

    void spinTransport(raccoon::Transport& transport, int maxIterations = 64)
    {
        for (int i = 0; i < maxIterations; ++i)
        {
            if (transport.spinOnce(0) <= 0)
            {
                return;
            }
        }

        throw TestFailure("transport queue did not drain within iteration budget");
    }

    void testTypedSubscriptionRecordsAndResetsLatencyStats()
    {
        auto transport = raccoon::Transport::create("memq://");

        int callbacks = 0;
        require(transport.subscribe<raccoon::scalar_i32_t>("hardware/motor/0/cmd/power",
            [&](const raccoon::scalar_i32_t& msg)
            {
                ++callbacks;
                require(msg.value == 123, "typed subscription should decode the published message");
            }),
            "typed subscribe failed");

        raccoon::scalar_i32_t msg;
        msg.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch() - std::chrono::milliseconds(5)).count();
        msg.value = 123;

        require(transport.publish("hardware/motor/0/cmd/power", msg), "typed publish failed");
        spinTransport(transport);

        require(callbacks == 1, "typed subscription should fire exactly once");

        auto stats = transport.getAndResetStats();
        require(stats.latency.count == 1, "typed delivery should record one latency sample");
        require(stats.callback.count == 1, "typed delivery should record one callback timing sample");
        require(stats.spin.count > 0, "spinning should record spin timing samples");
        require(stats.latency.minUs > 0, "latency minimum should be positive for a past timestamp");
        require(stats.latency.maxUs >= stats.latency.minUs, "latency max should be at least latency min");
        require(stats.latency.avgUs >= stats.latency.minUs, "latency avg should be at least latency min");
        require(!stats.channels.empty(), "per-channel stats should contain the subscribed channel");
        require(stats.channels.front().name == "hardware/motor/0/cmd/power",
                "per-channel stats should report the typed channel name");
        require(stats.channels.front().deliveries == 1,
                "per-channel stats should report exactly one delivery");
        require(stats.channels.front().latency.count == 1,
                "per-channel stats should include one latency sample");
        require(stats.channels.front().callback.count == 1,
                "per-channel stats should include one callback sample");

        auto reset = transport.getAndResetStats();
        require(reset.latency.count == 0, "latency stats should reset after retrieval");
        require(reset.callback.count == 0, "callback stats should reset after retrieval");
        require(reset.latency.minUs == 0, "reset latency min should be zeroed");
        require(reset.latency.maxUs == 0, "reset latency max should be zeroed");
        require(reset.latency.avgUs == 0, "reset latency avg should be zeroed");
        require(reset.channels.empty(), "per-channel stats should reset after retrieval");
    }

    // A frame that arrives while spinOnce() is parked can be delivered by the wait path instead of
    // the drain pass. Both must count it as a delivery, also when the payload is shorter than the
    // 8 bytes of a timestamp (there is no latency sample then). Repeated, because which path takes
    // a frame depends on the timing.
    void testParkedDeliveryOfSmallPayloadIsCounted()
    {
        constexpr int kFrames = 30;
        const std::string channel = "messages/warn";
        auto transport = raccoon::Transport::create();
        std::atomic<int> received{0};
        require(transport.subscribeRaw(channel,
                    [&](const void*, int) { ++received; }),
                "raw subscribe failed");

        for (int i = 0; i < kFrames; ++i)
        {
            std::thread publisher([&]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                const uint8_t payload[4] = {1, 2, 3, 4};
                transport.publishRaw(channel, payload, sizeof(payload));
            });

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (received.load() <= i && std::chrono::steady_clock::now() < deadline)
            {
                transport.spinOnce(500);
            }
            publisher.join();
            require(received.load() == i + 1, "every small frame should be delivered once (i=" +
                        std::to_string(i) + ", received=" + std::to_string(received.load()) + ")");
        }

        auto stats = transport.getAndResetStats();
        require(stats.channels.size() == 1 && stats.channels.front().name == channel,
                "per-channel stats should contain the channel");
        require(stats.channels.front().deliveries == kFrames,
                "every delivery should be counted, whichever path delivered it");
        require(stats.channels.front().callback.count == kFrames,
                "every delivery should record a callback sample");
    }

    void testHeartbeatStaysOnTimeWhileWritersFlood()
    {
        // Models the production layout: one daemon publishing at a fixed
        // 100 ms cadence ("heartbeat") plus N writers spamming unrelated
        // channels in parallel. Counts how many heartbeats land in 500 ms.
        // Without the mutex, the heartbeat publish could be interleaved with
        // a flood publish and silently fail. With the mutex it serializes
        // cleanly and the heartbeat count stays in the 4-6 window.
        constexpr int kFlooders     = 4;
        constexpr auto kRunDuration = std::chrono::milliseconds(550);

        auto transport = raccoon::Transport::create("memq://");

        std::atomic<int> heartbeatsSent{0};
        std::atomic<int> heartbeatsReceived{0};
        require(transport.subscribeRaw("messages/warn",
            [&](const void*, int)
            {
                heartbeatsReceived.fetch_add(1);
            }), "subscribe hb failed");

        std::atomic<bool> stop{false};

        std::thread heartbeat([&]
        {
            const uint8_t payload[1] = {0xBE};
            while (!stop.load())
            {
                if (transport.publishRaw("messages/warn", payload, sizeof(payload)))
                {
                    heartbeatsSent.fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        std::vector<std::thread> flooders;
        flooders.reserve(kFlooders);
        for (int w = 0; w < kFlooders; ++w)
        {
            flooders.emplace_back([&, w]
            {
                const uint8_t payload[2] = {static_cast<uint8_t>(w), 0xFF};
                while (!stop.load())
                {
                    transport.publishRaw("messages/error", payload, sizeof(payload));
                }
            });
        }

        std::this_thread::sleep_for(kRunDuration);
        stop.store(true);
        heartbeat.join();
        for (auto& t : flooders) t.join();

        spinTransport(transport, 8192);

        const int sent = heartbeatsSent.load();
        const int got  = heartbeatsReceived.load();
        require(sent >= 4,
                "heartbeat thread should have fired >=4 times, got " + std::to_string(sent));
        require(got >= sent - 1,
                "subscriber should have observed every published heartbeat; sent=" +
                    std::to_string(sent) + " got=" + std::to_string(got));
    }

    void testSubscriberCallbackCanPublishBack()
    {
        // The api mutex is recursive: a subscriber callback that turns
        // around and publishes (ack/echo pattern) must not self-deadlock.
        // A non-recursive mutex would lock the test thread forever.
        auto transport = raccoon::Transport::create("memq://");

        std::atomic<int> echoes{0};
        require(transport.subscribeRaw("camera/cmd/request",
            [&](const void*, int)
            {
                const uint8_t ack[1] = {0xAC};
                transport.publishRaw("camera/response", ack, sizeof(ack));
                echoes.fetch_add(1);
            }), "subscribe recurse failed");

        std::atomic<int> acks{0};
        require(transport.subscribeRaw("camera/response",
            [&](const void*, int) { acks.fetch_add(1); }), "subscribe ack failed");

        const uint8_t payload[1] = {0x42};
        require(transport.publishRaw("camera/cmd/request", payload, sizeof(payload)),
                "publish recurse failed");

        spinTransport(transport, 32);

        require(echoes.load() == 1, "subscriber callback should have fired exactly once");
        require(acks.load() == 1,
                "callback's nested publish should reach its own subscriber");
    }

    void testFrameSizesAndLimits()
    {
        // The slot size of a channel is fixed by the channel table. Frames up to the limit
        // pass, bigger ones are rejected and the channel keeps working.
        const std::string channel = "messages/error";
        auto transport = raccoon::Transport::create("memq://");

        std::mutex framesMutex;
        std::vector<std::vector<uint8_t>> frames;
        require(transport.subscribeRaw(channel,
            [&](const void* data, int len)
            {
                std::lock_guard<std::mutex> lk(framesMutex);
                const auto* bytes = static_cast<const uint8_t*>(data);
                frames.emplace_back(bytes, bytes + len);
            }), "subscribe failed");

        std::vector<uint8_t> small(64, 0xAB);
        require(transport.publishRaw(channel, small.data(), (int)small.size()), "small publish failed");

        std::vector<uint8_t> big(200000);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 31u);
        require(transport.publishRaw(channel, big.data(), (int)big.size()), "big publish failed");

        const auto* info = raccoon::channels::find_channel(channel);
        require(info != nullptr, "channel is in the table");
        std::vector<uint8_t> tooBig(info->payload + 1, 0x11);
        require(!transport.publishRaw(channel, tooBig.data(), (int)tooBig.size()),
                "a frame above the limit of the table must be rejected");

        std::vector<uint8_t> after(100, 0xCD);
        require(transport.publishRaw(channel, after.data(), (int)after.size()),
                "publish after a rejected frame failed");
        spinTransport(transport);

        std::lock_guard<std::mutex> lk(framesMutex);
        require(frames.size() == 3, "expected three frames, got " + std::to_string(frames.size()));
        require(frames[0] == small && frames[1] == big && frames[2] == after, "frames corrupt or reordered");
    }

    void testStrictChannelTable()
    {
        auto transport = raccoon::Transport::create("memq://");
        const uint8_t payload[12] = {};
        require(!transport.publishRaw("hardware/motor/9/cmd/power", payload, sizeof(payload)),
                "a channel that is not in the table must not be published");
        require(!transport.subscribeRaw("unit/anything", [](const void*, int) {}),
                "a channel that is not in the table must not be subscribed");
        // A fixed-size type has exactly one size.
        require(!transport.publishRaw("hardware/motor/0/cmd/power", payload, 11), "11 bytes on a 12-byte channel");
        require(!transport.publishRaw("hardware/motor/0/cmd/power", payload, 13), "13 bytes on a 12-byte channel");
        require(transport.publishRaw("hardware/motor/0/cmd/power", payload, 12), "12 bytes on a 12-byte channel");
    }

    void testSlowSubscriberCallbackDoesNotBlockConcurrentSubscribe()
    {
        auto transport = raccoon::Transport::create("memq://");

        std::atomic<bool> callbackEntered{false};
        std::atomic<bool> releaseCallback{false};
        std::atomic<bool> secondSubscribeInstalled{false};

        require(transport.subscribeRaw("messages/warn",
            [&](const void*, int)
            {
                callbackEntered.store(true);
                while (!releaseCallback.load())
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }), "subscribe slow failed");

        std::thread spinner([&] { transport.spin(); });

        const uint8_t payload[1] = {0x42};
        require(transport.publishRaw("messages/warn", payload, sizeof(payload)),
                "publish slow failed");

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!callbackEntered.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(callbackEntered.load(), "slow callback should have started");

        auto subscribeFuture = std::async(std::launch::async, [&]
        {
            bool ok = transport.subscribeRaw("messages/error",
                [&](const void*, int) {}, raccoon::SubscribeOptions{});
            secondSubscribeInstalled.store(ok);
            return ok;
        });

        auto futureStatus = subscribeFuture.wait_for(std::chrono::milliseconds(50));
        releaseCallback.store(true);

        transport.stop();
        spinner.join();

        require(futureStatus == std::future_status::ready,
                "concurrent subscribe should not wait for a slow callback to finish");
        require(subscribeFuture.get(), "late subscribe should succeed");
        require(secondSubscribeInstalled.load(), "late subscribe should be installed");
    }

    void testValueChannelDeduplicatesIdenticalPayloads()
    {
        // A value channel with deduplicate=true must drop publishes whose
        // value is unchanged, even though each carries a fresh timestamp
        // (the dedup comparison excludes the leading 8-byte stamp).
        // A retained channel hands its latest frame to a new subscriber: start from an empty ring.
        resetRing("hardware/motor/1/state/power");
        auto transport = raccoon::Transport::create("memq://");
        const std::string channel = "hardware/motor/1/state/power";
        require(!raccoon::Channels::isCommandChannel(channel),
                "value channel must not classify as a command channel");

        int callbacks = 0;
        int lastValue = 0;
        require(transport.subscribe<raccoon::scalar_i32_t>(channel,
            [&](const raccoon::scalar_i32_t& msg)
            {
                ++callbacks;
                lastValue = msg.value;
            }),
            "value subscribe failed");

        const raccoon::PublishOptions dedup{.deduplicate = true};

        raccoon::scalar_i32_t a;
        a.timestamp = 1000;  // distinct timestamps prove the stamp is ignored
        a.value = 42;
        require(transport.publish(channel, a, dedup), "first value publish failed");

        raccoon::scalar_i32_t b;
        b.timestamp = 2000;
        b.value = 42;  // same value -> must be deduplicated
        require(transport.publish(channel, b, dedup), "second value publish failed");

        spinTransport(transport);
        require(callbacks == 1, "identical value should be delivered only once");

        auto stats = transport.getAndResetStats();
        require(stats.publishesDeduplicated == 1,
                "one publish should be counted as deduplicated");

        raccoon::scalar_i32_t c;
        c.timestamp = 3000;
        c.value = 43;  // changed value -> must be delivered
        require(transport.publish(channel, c, dedup), "third value publish failed");
        spinTransport(transport);
        require(callbacks == 2, "changed value should be delivered");
        require(lastValue == 43, "subscriber should see the changed value");
    }

    void testCommandChannelNeverDeduplicates()
    {
        // A command channel must deliver every publish even with
        // deduplicate=true and a byte-identical payload — re-issuing the
        // same command is meaningful and must reach the subscriber.
        auto transport = raccoon::Transport::create("memq://");
        const std::string channel = "hardware/motor/2/cmd/power";
        require(raccoon::Channels::isCommandChannel(channel),
                "channel ending in _cmd must classify as a command channel");

        int callbacks = 0;
        require(transport.subscribe<raccoon::scalar_i32_t>(channel,
            [&](const raccoon::scalar_i32_t&) { ++callbacks; }),
            "command subscribe failed");

        const raccoon::PublishOptions dedup{.deduplicate = true};
        raccoon::scalar_i32_t cmd;
        cmd.timestamp = 5000;
        cmd.value = 7;
        require(transport.publish(channel, cmd, dedup), "first command publish failed");
        require(transport.publish(channel, cmd, dedup), "second command publish failed");

        spinTransport(transport);
        require(callbacks == 2, "identical commands must both be delivered");

        auto stats = transport.getAndResetStats();
        require(stats.publishesDeduplicated == 0,
                "command channel must never count deduplicated publishes");
    }
}  // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"typed subscription records and resets latency stats", testTypedSubscriptionRecordsAndResetsLatencyStats},
        {"value channel deduplicates identical payloads", testValueChannelDeduplicatesIdenticalPayloads},
        {"command channel never deduplicates", testCommandChannelNeverDeduplicates},
        {"parked delivery of a small payload is counted", testParkedDeliveryOfSmallPayloadIsCounted},
        {"heartbeat stays on time while writers flood", testHeartbeatStaysOnTimeWhileWritersFlood},
        {"subscriber callback can publish back (recursive)", testSubscriberCallbackCanPublishBack},
        {"frame sizes follow the channel table", testFrameSizesAndLimits},
        {"only channels of the table exist", testStrictChannelTable},
        {"slow subscriber callback does not block concurrent subscribe", testSlowSubscriberCallbackDoesNotBlockConcurrentSubscribe},
    };

    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
            return 1;
        }
    }

    return 0;
}
