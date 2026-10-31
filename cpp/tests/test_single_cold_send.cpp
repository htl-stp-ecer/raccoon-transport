// Regression test: a SINGLE publish to a channel that has never existed
// before must reach a subscriber that started before the producer.
//
// Motivation: the cone-pusher lower command (raccoon/motor/3/velocity_cmd)
// was published exactly once, on a channel used for the first and only time
// in the whole run, and never reached the stm32-data-reader. SHM is lossless
// and retains the last slot, so this MUST be delivered. These tests pin that
// contract, including the hard case where the subscriber is parked in a
// blocking futex wait on OTHER (warm) channels and no further traffic arrives
// to opportunistically wake it.

#include "raccoon/Transport.h"
#include "raccoon/raccoon_ring.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>
#include <sys/wait.h>

// PublishOptions{} touches the legacy `reliable`/`retryInterval` members,
// which are deprecated no-ops on the SHM backend. We publish with defaults on
// purpose (best-effort is all the SHM ring offers); silence the note here just
// like Transport.cpp does at its own call sites.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace
{
    struct TestFailure : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    void require(bool condition, const std::string& message)
    {
        if (!condition) throw TestFailure(message);
    }

    // Unlink the /dev/shm files for a channel before and after a test so
    // runs are independent (mirrors ChannelCleanup in test_producer_restart).
    class ChannelCleanup
    {
    public:
        explicit ChannelCleanup(std::string channel) : channel_(std::move(channel))
        {
            require(rrb_channel_to_path(channel_.c_str(), path_, sizeof(path_)) == 0,
                    "failed to derive shm path for " + channel_);
            (void)unlink(path_);
        }
        ~ChannelCleanup() { (void)unlink(path_); }
        ChannelCleanup(const ChannelCleanup&) = delete;
        ChannelCleanup& operator=(const ChannelCleanup&) = delete;

    private:
        std::string channel_;
        char path_[512]{};
    };

    // A reader whose spin cadence is configurable. blockingTimeoutMs > 0
    // makes each spinOnce() park in the kernel (the futex_waitv path) exactly
    // like the stm32-data-reader's LcmBroker does — that is where a cold
    // channel can be missed, because a lazy (unattached) reader is not in the
    // waitv list and the producer's first publish skips FUTEX_WAKE.
    class ReaderPump
    {
    public:
        ReaderPump(const std::vector<std::string>& channels, int blockingTimeoutMs)
            : blockingTimeoutMs_(blockingTimeoutMs)
        {
            transport_ = raccoon::Transport::create();
            for (const auto& ch : channels)
            {
                const std::string channel = ch;
                require(transport_.subscribeRaw(channel,
                    [this, channel](const void* data, int len)
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        counts_[channel] += 1;
                        last_[channel].assign(static_cast<const char*>(data),
                                              static_cast<size_t>(len));
                    }),
                    "failed to subscribe reader for " + channel);
            }
            worker_ = std::thread([this]() { run(); });
        }

        ~ReaderPump()
        {
            stop_ = true;
            transport_.wakeControl();  // break any in-flight blocking wait
            if (worker_.joinable()) worker_.join();
            transport_.shutdown();
        }

        int count(const std::string& channel) const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = counts_.find(channel);
            return it == counts_.end() ? 0 : it->second;
        }

        bool waitForCount(const std::string& channel, int want,
                          std::chrono::milliseconds timeout) const
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (count(channel) >= want) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return count(channel) >= want;
        }

    private:
        void run()
        {
            while (!stop_)
            {
                if (blockingTimeoutMs_ > 0)
                {
                    transport_.spinOnce(blockingTimeoutMs_);
                }
                else if (transport_.spinOnce(0) <= 0)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        raccoon::Transport transport_;
        mutable std::mutex mutex_;
        std::unordered_map<std::string, int> counts_;
        std::unordered_map<std::string, std::string> last_;
        std::thread worker_;
        std::atomic<bool> stop_{false};
        int blockingTimeoutMs_;
    };

    // Publish exactly one message through the SAME high-level API the robot
    // uses (raccoon::Transport::publishRaw), so the test exercises the real
    // writer-create-on-first-publish path, not a hand-made rrb_writer.
    void publishOnce(const std::string& channel, const std::string& payload)
    {
        raccoon::Transport pub = raccoon::Transport::create();
        require(pub.publishRaw(channel, payload.data(),
                               static_cast<int>(payload.size()), {}),
                "publishRaw failed for " + channel);
        // Keep the writer (and its /dev/shm ring) alive briefly so the ring
        // is not torn down before the reader attaches — the robot process
        // keeps its writer for the whole run.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    // Helper that publishes one keepalive frame and holds the writer open for
    // the lifetime of the test via a leaked-on-purpose static so the warm ring
    // stays materialised.
    void publishOnceKeepalive(const std::string& channel, const std::string& payload)
    {
        static std::vector<raccoon::Transport> keep;
        keep.emplace_back(raccoon::Transport::create());
        require(keep.back().publishRaw(channel, payload.data(),
                                       static_cast<int>(payload.size()), {}),
                "publishRaw failed for " + channel);
    }

    // Case 1: 1 ms poll loop (matches LcmBroker's spinOnce(1) cadence).
    void testSingleColdSendPolling()
    {
        const std::string channel = "raccoon/test/single_cold_poll";
        ChannelCleanup cleanup(channel);

        ReaderPump reader({channel}, /*blockingTimeoutMs=*/0);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // let it park lazy

        publishOnce(channel, "only-one");

        require(reader.waitForCount(channel, 1, std::chrono::seconds(2)),
                "single cold-channel send was NOT delivered (polling reader)");
    }

    // Case 2: the run scenario. Reader is subscribed to a WARM channel (which
    // it attaches to and parks on) plus the COLD channel. It uses a blocking
    // spin. We publish once on the cold channel and then send NO further
    // traffic on the warm channel, so nothing opportunistically wakes the
    // parked thread. The cold single send must still be delivered.
    void testSingleColdSendBlockingNoOtherTraffic()
    {
        const std::string warm = "raccoon/test/warm_channel";
        const std::string cold = "raccoon/test/single_cold_blocking";
        ChannelCleanup cleanupWarm(warm);
        ChannelCleanup cleanupCold(cold);

        // Establish the warm channel first so the reader attaches + parks on it.
        publishOnceKeepalive(warm, "warm-0");

        ReaderPump reader({warm, cold}, /*blockingTimeoutMs=*/2000);
        require(reader.waitForCount(warm, 1, std::chrono::seconds(2)),
                "warm channel priming frame not delivered");

        // Give the reader time to re-enter a long blocking wait on the warm
        // channel's wake_seq. The cold channel is still lazy (base == NULL),
        // so it is NOT in the futex_waitv list.
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        publishOnce(cold, "cold-only");

        require(reader.waitForCount(cold, 1, std::chrono::seconds(3)),
                "single cold-channel send was NOT delivered while reader parked "
                "on an idle warm channel (wakeup gap)");
    }

    // Case 3: the faithful deployment topology — writer and reader are in
    // SEPARATE processes (robot vs. stm32-data-reader), talking only through
    // /dev/shm. The reader (child) subscribes first and keeps polling; the
    // writer (parent) publishes exactly one frame on the cold channel after a
    // delay. The child exits 0 iff it received the frame.
    void testSingleColdSendCrossProcess()
    {
        const std::string channel = "raccoon/test/single_cold_xproc";
        ChannelCleanup cleanup(channel);

        pid_t pid = fork();
        require(pid >= 0, "fork failed");

        if (pid == 0)
        {
            // Child = reader. Subscribe to the (still non-existent) channel,
            // poll at 1 ms like the real LcmBroker, exit 0 on first frame.
            std::atomic<bool> got{false};
            raccoon::Transport rx = raccoon::Transport::create();
            rx.subscribeRaw(channel,
                [&got](const void*, int) { got.store(true); });
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!got.load() && std::chrono::steady_clock::now() < deadline)
            {
                if (rx.spinOnce(1) <= 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            rx.shutdown();
            _exit(got.load() ? 0 : 1);
        }

        // Parent = writer. Let the child subscribe + park first, then publish
        // exactly once.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        publishOnce(channel, "xproc-one");

        int status = 0;
        require(waitpid(pid, &status, 0) == pid, "waitpid failed");
        require(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                "cross-process single cold-channel send was NOT delivered");
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

int main(int argc, char** argv)
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"test_single_cold_send_polling", testSingleColdSendPolling},
        {"test_single_cold_send_blocking_no_other_traffic",
         testSingleColdSendBlockingNoOtherTraffic},
        {"test_single_cold_send_cross_process", testSingleColdSendCrossProcess},
    };

    if (argc > 1)
    {
        const std::string wanted = argv[1];
        for (const auto& [name, fn] : tests)
        {
            if (wanted == name) { runTest(name, fn); return 0; }
        }
        std::fprintf(stderr, "unknown test %s\n", argv[1]);
        return 2;
    }

    for (const auto& [name, fn] : tests) runTest(name, fn);
    return 0;
}
