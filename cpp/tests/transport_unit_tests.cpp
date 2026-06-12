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
        require(transport.subscribe<raccoon::scalar_i32_t>("unit/typed",
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

        require(transport.publish("unit/typed", msg), "typed publish failed");
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
        require(stats.channels.front().name == "unit/typed",
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
        require(transport.subscribeRaw("unit/hb",
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
                if (transport.publishRaw("unit/hb", payload, sizeof(payload)))
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
                    transport.publishRaw("unit/flood", payload, sizeof(payload));
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
        require(transport.subscribeRaw("unit/recurse",
            [&](const void*, int)
            {
                const uint8_t ack[1] = {0xAC};
                transport.publishRaw("unit/recurse-ack", ack, sizeof(ack));
                echoes.fetch_add(1);
            }), "subscribe recurse failed");

        std::atomic<int> acks{0};
        require(transport.subscribeRaw("unit/recurse-ack",
            [&](const void*, int) { acks.fetch_add(1); }), "subscribe ack failed");

        const uint8_t payload[1] = {0x42};
        require(transport.publishRaw("unit/recurse", payload, sizeof(payload)),
                "publish recurse failed");

        spinTransport(transport, 32);

        require(echoes.load() == 1, "subscriber callback should have fired exactly once");
        require(acks.load() == 1,
                "callback's nested publish should reach its own subscriber");
    }

    void testOversizedPublishGrowsRingAndRepublishes()
    {
        // Regression test for the grow-and-republish path: a payload larger
        // than the ring's max_payload must transparently re-create the ring
        // with a bigger slot size AND republish the frame — and an already-
        // attached subscriber must keep receiving (the reader detects the
        // slot_size change and re-attaches; this detection was lost once in
        // a refactor and froze the botui on the last pre-grow frame).
        const std::string channel = "unit/grow";

        // Wipe any stale ring from a previous run so the first publish
        // deterministically sizes the ring at RRB_DEFAULT_MAX_PAYLOAD.
        char shmPath[512];
        require(rrb_channel_to_path(channel.c_str(), shmPath, sizeof(shmPath)) == 0,
                "failed to derive shm path for " + channel);
        (void)unlink(shmPath);

        auto transport = raccoon::Transport::create("memq://");

        std::mutex framesMutex;
        std::vector<std::vector<uint8_t>> frames;
        require(transport.subscribeRaw(channel,
            [&](const void* data, int len)
            {
                std::lock_guard<std::mutex> lk(framesMutex);
                const auto* bytes = static_cast<const uint8_t*>(data);
                frames.emplace_back(bytes, bytes + len);
            }), "subscribe grow failed");

        // First publish sizes the ring at the 2 KiB default.
        std::vector<uint8_t> small(64, 0xAB);
        require(transport.publishRaw(channel, small.data(), (int)small.size()),
                "small publish failed");
        spinTransport(transport);

        // Oversized frame (> 2 KiB) forces grow-and-republish.
        std::vector<uint8_t> big(8000);
        for (size_t i = 0; i < big.size(); ++i)
        {
            big[i] = static_cast<uint8_t>(i * 31u);
        }
        require(transport.publishRaw(channel, big.data(), (int)big.size()),
                "oversized publish should grow the ring and succeed");
        spinTransport(transport);

        // The channel must keep working after the resize.
        std::vector<uint8_t> after(100, 0xCD);
        require(transport.publishRaw(channel, after.data(), (int)after.size()),
                "post-grow publish failed");
        spinTransport(transport);

        std::lock_guard<std::mutex> lk(framesMutex);
        bool sawBig = false;
        bool sawAfter = false;
        for (const auto& f : frames)
        {
            if (f == big) sawBig = true;
            if (f == after) sawAfter = true;
        }
        require(sawBig,
                "subscriber should receive the oversized frame intact after the ring grow");
        require(sawAfter,
                "subscriber should keep receiving frames published after the ring grow");
    }

    void testSlowSubscriberCallbackDoesNotBlockConcurrentSubscribe()
    {
        auto transport = raccoon::Transport::create("memq://");

        std::atomic<bool> callbackEntered{false};
        std::atomic<bool> releaseCallback{false};
        std::atomic<bool> secondSubscribeInstalled{false};

        require(transport.subscribeRaw("unit/slow",
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
        require(transport.publishRaw("unit/slow", payload, sizeof(payload)),
                "publish slow failed");

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (!callbackEntered.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        require(callbackEntered.load(), "slow callback should have started");

        auto subscribeFuture = std::async(std::launch::async, [&]
        {
            bool ok = transport.subscribeRaw("unit/late",
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
}  // namespace

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"typed subscription records and resets latency stats", testTypedSubscriptionRecordsAndResetsLatencyStats},
        {"heartbeat stays on time while writers flood", testHeartbeatStaysOnTimeWhileWritersFlood},
        {"subscriber callback can publish back (recursive)", testSubscriberCallbackCanPublishBack},
        {"oversized publish grows ring and republishes", testOversizedPublishGrowsRingAndRepublishes},
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
