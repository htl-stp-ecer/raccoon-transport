// Regression + contract tests for reliable (at-least-once) delivery on the
// raccoon_ring SHM backend.
//
// Motivation: the cone-pusher lower command
// (raccoon/motor/3/velocity_cmd = -1300) was published best-effort exactly
// once, lost in transport, and the motor sat still — the mode/brake that
// followed WAS delivered, so the failure was silent. `PublishOptions.reliable`
// existed but was a no-op. These tests pin the new contract:
//   1. a reliable command is re-sent until the subscriber ACKs it,
//   2. re-sends are de-duplicated (handler fires once per unique command),
//   3. with no subscriber the publisher gives up after maxRetries and says so,
//   4. the whole loop closes ACROSS PROCESSES (robot ↔ stm32-data-reader).

#include "raccoon/Transport.h"
#include "raccoon/Channels.h"
#include "raccoon/raccoon_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>

namespace
{
    struct TestFailure : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    void require(bool cond, const std::string& msg)
    {
        if (!cond) throw TestFailure(msg);
    }

    void unlinkChannel(const std::string& channel)
    {
        char path[512]{};
        if (rrb_channel_to_path(channel.c_str(), path, sizeof(path)) == 0)
            (void)unlink(path);
    }

    // A command payload: 8-byte big-endian timestamp (the reliability layer's
    // correlation key, matching every raccoon message's leading `timestamp`
    // field) followed by a little value tail.
    std::vector<uint8_t> makePayload(int64_t ts, int32_t value)
    {
        std::vector<uint8_t> p(12, 0);
        for (int i = 0; i < 8; ++i)
            p[i] = static_cast<uint8_t>((ts >> (8 * (7 - i))) & 0xFF);
        std::memcpy(p.data() + 8, &value, 4);
        return p;
    }

    // Spins a transport in a background thread until stopped.
    class Spinner
    {
    public:
        explicit Spinner(raccoon::Transport& t) : t_(t)
        {
            worker_ = std::thread([this] {
                while (!stop_.load()) t_.spinOnce(20);
            });
        }
        ~Spinner()
        {
            stop_.store(true);
            t_.wakeControl();
            if (worker_.joinable()) worker_.join();
        }

    private:
        raccoon::Transport& t_;
        std::thread worker_;
        std::atomic<bool> stop_{false};
    };

    template <typename Pred>
    bool waitFor(Pred pred, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return pred();
    }

    const std::string kChannel = "raccoon/motor/3/velocity_cmd";
    const std::string kAck = raccoon::Channels::Protocol::ACK;

    void cleanAll()
    {
        unlinkChannel(kChannel);
        unlinkChannel(kAck);
    }

    // 1. Happy path: reliable command is delivered exactly once and ACKed,
    //    so the publisher stops retransmitting (dropped == 0).
    void testReliableDeliveredAndAcked()
    {
        cleanAll();
        raccoon::Transport sub = raccoon::Transport::create();
        std::atomic<int> count{0};
        require(sub.subscribeRaw(kChannel,
                    [&count](const void*, int) { count.fetch_add(1); },
                    raccoon::SubscribeOptions{.reliable = true}),
                "subscribe failed");
        Spinner subSpin(sub);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        raccoon::Transport pub = raccoon::Transport::create();
        auto payload = makePayload(/*ts=*/1000, /*value=*/-1300);
        raccoon::PublishOptions po;
        po.reliable = true;
        po.retryInterval = std::chrono::milliseconds(20);
        po.maxRetries = 10;
        require(pub.publishRaw(kChannel, payload.data(),
                               static_cast<int>(payload.size()), po),
                "publish failed");

        require(waitFor([&] { return count.load() >= 1; },
                        std::chrono::seconds(2)),
                "reliable command was not delivered");
        // Let the ACK propagate and any in-flight retransmit settle.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        require(count.load() == 1,
                "reliable command delivered " + std::to_string(count.load()) +
                    " times, expected exactly 1 (dedup broken)");
        auto stats = pub.getAndResetStats();
        require(stats.reliableDropped == 0,
                "publisher gave up on an ACKed command (dropped=" +
                    std::to_string(stats.reliableDropped) + ")");
    }

    // 2. Dedup under forced retransmission: the subscriber starts draining
    //    only AFTER several retransmits have piled up. The handler must still
    //    fire exactly once, and at least one retransmit must have happened.
    void testReliableDedupUnderRetransmit()
    {
        cleanAll();
        raccoon::Transport sub = raccoon::Transport::create();
        std::atomic<int> count{0};
        require(sub.subscribeRaw(kChannel,
                    [&count](const void*, int) { count.fetch_add(1); },
                    raccoon::SubscribeOptions{.reliable = true}),
                "subscribe failed");
        // NOTE: not spinning `sub` yet — ACKs cannot flow, so the publisher
        // is forced to retransmit.

        raccoon::Transport pub = raccoon::Transport::create();
        auto payload = makePayload(/*ts=*/2000, /*value=*/-1300);
        raccoon::PublishOptions po;
        po.reliable = true;
        po.retryInterval = std::chrono::milliseconds(20);
        po.maxRetries = 50;
        require(pub.publishRaw(kChannel, payload.data(),
                               static_cast<int>(payload.size()), po),
                "publish failed");

        // Let the retry thread fire several retransmits with no ACK.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));

        // Now let the subscriber drain: it will see many identical copies but
        // must deliver to the handler exactly once and ACK them.
        Spinner subSpin(sub);
        require(waitFor([&] { return count.load() >= 1; },
                        std::chrono::seconds(2)),
                "buffered reliable command was not delivered once draining");
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        require(count.load() == 1,
                "reliable command delivered " + std::to_string(count.load()) +
                    " times despite dedup, expected exactly 1");
        auto stats = pub.getAndResetStats();
        require(stats.reliableRetransmits >= 1,
                "expected at least one retransmit while subscriber was silent");
    }

    // 3. Bounded give-up: with no subscriber, the publisher retries maxRetries
    //    times and then drops the command, incrementing reliableDropped (the
    //    loud, chase-worthy visibility signal).
    void testReliableGivesUpWithoutSubscriber()
    {
        cleanAll();
        raccoon::Transport pub = raccoon::Transport::create();
        auto payload = makePayload(/*ts=*/3000, /*value=*/-1300);
        raccoon::PublishOptions po;
        po.reliable = true;
        po.retryInterval = std::chrono::milliseconds(20);
        po.maxRetries = 3;
        require(pub.publishRaw(kChannel, payload.data(),
                               static_cast<int>(payload.size()), po),
                "publish failed");

        require(waitFor(
                    [&] {
                        auto s = pub.getAndResetStats();
                        return s.reliableDropped >= 1;
                    },
                    std::chrono::seconds(2)),
                "publisher never gave up on an undeliverable reliable command");
    }

    // 4. Cross-process, the real topology: child = reliable subscriber, parent
    //    = reliable publisher. Child exits 0 iff it received the command; the
    //    parent then confirms it did NOT have to give up (the ACK crossed the
    //    process boundary).
    void testReliableCrossProcess()
    {
        cleanAll();
        pid_t pid = fork();
        require(pid >= 0, "fork failed");

        if (pid == 0)
        {
            raccoon::Transport rx = raccoon::Transport::create();
            std::atomic<bool> got{false};
            rx.subscribeRaw(kChannel,
                            [&got](const void*, int) { got.store(true); },
                            raccoon::SubscribeOptions{.reliable = true});
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!got.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                rx.spinOnce(20);
            }
            // Keep spinning briefly so the ACK is actually flushed to the
            // publisher before we exit.
            for (int i = 0; i < 20; ++i) rx.spinOnce(5);
            rx.shutdown();
            _exit(got.load() ? 0 : 1);
        }

        // Parent = publisher. Let the child subscribe + park, then publish once.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        raccoon::Transport pub = raccoon::Transport::create();
        auto payload = makePayload(/*ts=*/4000, /*value=*/-1300);
        raccoon::PublishOptions po;
        po.reliable = true;
        po.retryInterval = std::chrono::milliseconds(30);
        po.maxRetries = 100;
        require(pub.publishRaw(kChannel, payload.data(),
                               static_cast<int>(payload.size()), po),
                "publish failed");

        int status = 0;
        require(waitpid(pid, &status, 0) == pid, "waitpid failed");
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "cross-process reliable command was NOT received by child");

        // The child ACKed before exiting; give the parent's retry thread a
        // moment to consume it, then confirm no give-up occurred.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto stats = pub.getAndResetStats();
        require(stats.reliableDropped == 0,
                "publisher gave up despite the child receiving the command");
    }

    void runTest(const char* name, const std::function<void()>& fn)
    {
        try
        {
            fn();
            std::fprintf(stderr, "[PASS] %s\n", name);
        }
        catch (const std::exception& ex)
        {
            std::fprintf(stderr, "[FAIL] %s: %s\n", name, ex.what());
            throw;
        }
    }
}

int main()
{
    runTest("test_reliable_delivered_and_acked", testReliableDeliveredAndAcked);
    runTest("test_reliable_dedup_under_retransmit",
            testReliableDedupUnderRetransmit);
    runTest("test_reliable_gives_up_without_subscriber",
            testReliableGivesUpWithoutSubscriber);
    runTest("test_reliable_cross_process", testReliableCrossProcess);
    cleanAll();
    return 0;
}
