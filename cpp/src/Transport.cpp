// raccoon::Transport — iceoryx2 backend.
//
// History: this used to be a thin wrapper around lcm::LCM that ran the
// pub/sub bytes over UDP multicast on the loopback interface. On a Pi 3B
// that collapsed under the production load (25k pkts/s × 3+ subscribers ⇒
// 55 % packet loss, 200 % CPU). The benchmark in raccoon-lib's history
// commit shows iceoryx2 holding the same load at 0 % loss / 68 % CPU.
//
// The wire format (LCM-generated encode/decode bytes for each message
// type) is unchanged — those bytes are now shipped through an iceoryx2
// shared-memory pub/sub service per channel instead of through LCM UDP.
// The public API surface (publish<T>, subscribe<T>, publishRaw,
// subscribeRaw, spinOnce, spin, getAndResetStats) is unchanged.
//
// Behavior changes versus the LCM backend (intentional):
//
//   * `PublishOptions::reliable` is silently ignored. LCM needed an
//     at-least-once retry protocol because UDP-multicast on loopback
//     dropped packets under load (the 638 RcvbufErrors that triggered
//     this migration in the first place). iceoryx2 routes through
//     shared memory, so kernel-level drops don't exist; the only
//     remaining loss source is a subscriber-queue overflow, which the
//     buffer_size=64 + retain-1 below already prevents for the command
//     channels that ever set `reliable=true` (motor mode_cmd,
//     servo smooth_cmd, kinematics_config_cmd, …). The old reliable
//     warning was removed because the no-op is now the *correct* behavior.
//
//   * `history_size=1` on every service, regardless of
//     `PublishOptions::retained` — cheap and removes the old
//     RETAIN_REQUEST control-channel handshake entirely.
//     Semantic caveat: iceoryx2 history is publisher-side. A late
//     subscriber receives the historical sample on the publisher's
//     NEXT send(). If the publisher has died or is idle, no replay
//     happens. This is acceptable for raccoon's actual usage pattern
//     — sensor streams keep publishing, and command consumers
//     (stm32 CommandSubscriber) are always running before commands
//     fly. If a future caller needs durable cross-process retention,
//     we'd need either a long-lived "retain daemon" node or a flat
//     file mirror — both are out of scope here.
//
//   * Payload is a dynamic-size iceoryx2 `bb::Slice<uint8_t>`. Every
//     publish allocates exactly `dataLen` bytes from the publisher's
//     SHM pool, growing the pool in PowerOfTwo steps from a 4 KB
//     starting hint. cam_frame_t / screen_render_t / yolo_frame_t fit
//     fine; small scalar messages don't waste a fixed slot anymore.
#include "raccoon/Transport.h"
#include "raccoon/Channels.h"

#include "iox2/iceoryx2.hpp"

// Suppress our own deprecation warning on `reliable` / `retryInterval` /
// `maxRetries` — this file is the place that *forwards* the value into
// a no-op, which is exactly what the deprecation tells external callers
// to stop doing. Internal pass-through is fine.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace raccoon
{
    // The wire payload is a dynamic-size byte slice (iceoryx2 `bb::Slice<uint8_t>`).
    // Each publish allocates exactly the message size; the publisher's SHM
    // pool grows in PowerOfTwo steps from this starting hint. Sized so the
    // common scalar/vector LCM messages (≤ ~64 B encoded) fit without realloc;
    // larger payloads (cam_frame_t, screen_render_t) trigger one or two
    // PowerOfTwo doublings.
    static constexpr uint64_t kInitialSliceHint = 4096;

    using IoxPayload    = iox2::bb::Slice<uint8_t>;
    using IoxNode       = iox2::Node<iox2::ServiceType::Ipc>;
    using IoxService    = iox2::PortFactoryPublishSubscribe<iox2::ServiceType::Ipc, IoxPayload, void>;
    using IoxPublisher  = iox2::Publisher<iox2::ServiceType::Ipc, IoxPayload, void>;
    using IoxSubscriber = iox2::Subscriber<iox2::ServiceType::Ipc, IoxPayload, void>;

    static int64_t percentile99(std::vector<int64_t> samples)
    {
        if (samples.empty()) return 0;
        const auto index = static_cast<size_t>(
            std::ceil(static_cast<double>(samples.size()) * 0.99)) - 1;
        const auto nth = std::min(index, samples.size() - 1);
        std::nth_element(samples.begin(),
                         samples.begin() + static_cast<std::ptrdiff_t>(nth),
                         samples.end());
        return samples[nth];
    }

    // iceoryx2 service names share LCM's channel naming (slash-separated).
    // Empty names and control characters are rejected by iceoryx2; on
    // failure we return nullopt so the caller can refuse the publish/
    // subscribe instead of aborting via a panicking .value() unwrap.
    static auto tryMakeServiceName(const std::string& channel)
        -> std::optional<iox2::ServiceName>
    {
        auto result = iox2::ServiceName::create(channel.c_str());
        if (!result.has_value()) return std::nullopt;
        return std::optional<iox2::ServiceName>{std::move(result.value())};
    }

    // Retry policy for transient iox2 open_or_create errors.
    //
    // iox2 docs name three specific failure classes as "retry-after-a-delay":
    //   * OpenIsMarkedForDestruction        (mid-cleanup race)
    //   * IsBeingCreatedByAnotherInstance   (concurrent peer creating)
    //   * OpenServiceInCorruptedState       (cleanup-after-reboot path)
    //
    // The third one is the slow one — when a process died uncleanly and
    // left `/tmp/iceoryx2/services/*.service` behind, the next iox2 node
    // detects the orphan, calls `remove_node_from_service` to scrub it,
    // and the cleanup is on the order of *seconds*, not ms. A short
    // retry budget would turn that healing pass into a clean failure
    // that the operator has to recover from manually.
    //
    // Budget: ~3 s total via exponential backoff (20 ms → 320 ms cap),
    // so the cheap transients are handled in tens of ms while the
    // post-reboot corruption-cleanup case has time to complete without
    // operator intervention. The first transient gets logged once so
    // a slow startup is visible instead of looking like a hang.
    static constexpr int kIoxRetryAttempts = 20;
    static constexpr std::chrono::milliseconds kIoxRetryInitialDelay{20};
    static constexpr std::chrono::milliseconds kIoxRetryMaxDelay{320};

    template <typename F>
    static auto retryIox(F&& fn, const char* operation = nullptr)
    {
        const auto started = std::chrono::steady_clock::now();
        auto delay = kIoxRetryInitialDelay;
        bool announcedTransient = false;
        auto last = fn();
        for (int attempt = 1; attempt < kIoxRetryAttempts; ++attempt)
        {
            if (last.has_value())
            {
                if (announcedTransient && operation)
                {
                    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started).count();
                    std::cerr << "raccoon::Transport: " << operation
                              << " recovered after " << waited << " ms\n";
                }
                return last;
            }
            if (!announcedTransient && operation)
            {
                std::cerr << "raccoon::Transport: " << operation
                          << " hit a transient iceoryx2 error — retrying "
                          << "(post-reboot cleanup can take a few seconds)\n";
                announcedTransient = true;
            }
            std::this_thread::sleep_for(delay);
            delay = std::min(delay * 2, kIoxRetryMaxDelay);
            last = fn();
        }
        if (!last.has_value() && operation)
        {
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::cerr << "raccoon::Transport: " << operation
                      << " did not recover after " << waited << " ms.\n"
                      << "  This usually means another iox2 process is holding\n"
                      << "  a corrupted service. Self-heal procedure:\n"
                      << "    sudo systemctl stop stm32_data_reader.service\n"
                      << "    sudo pkill -f raccoon_cli.server\n"
                      << "    sudo rm -rf /tmp/iceoryx2 /dev/shm/iox2_*\n"
                      << "    sudo systemctl start stm32_data_reader.service\n"
                      << "  and rerun. If this keeps happening on every boot,\n"
                      << "  check that no service is launching as root\n"
                      << "  (root-owned iox2 state blocks pi-user processes).\n";
        }
        return last;
    }

    class Transport::Impl
    {
    public:
        std::unique_ptr<IoxNode> node;
        std::atomic<bool> running{false};

        struct PubEntry
        {
            std::unique_ptr<IoxService>   service;
            std::unique_ptr<IoxPublisher> publisher;
        };
        struct SubEntry
        {
            std::string channel;
            bool retained{false};
            std::unique_ptr<IoxService>    service;
            std::unique_ptr<IoxSubscriber> subscriber;
            std::vector<Transport::RawHandler> handlers;
        };

        std::unordered_map<std::string, PubEntry>   publishers;
        std::vector<std::unique_ptr<SubEntry>>      subscribers;

        // Deduplication cache (unchanged from LCM version).
        std::unordered_map<std::string, std::vector<uint8_t>> deduplicationCache;

        // One coarse mutex protects every Impl mutation. We do not run iceoryx2
        // pub/sub ports from multiple threads — that would be racy per the
        // iceoryx2 contract — so this mutex doubles as the per-port serialiser.
        // Recursive so a callback that publishes back does not deadlock.
        std::recursive_mutex apiMutex;

        // ---- Stats ---------------------------------------------------------
        struct ChannelStatsAccumulator
        {
            uint64_t deliveries{0};
            int64_t latencyMinUs{std::numeric_limits<int64_t>::max()};
            int64_t latencyMaxUs{0};
            int64_t latencySumUs{0};
            uint64_t latencyCount{0};
            std::vector<int64_t> latencySamplesUs{};
            int64_t callbackMinUs{std::numeric_limits<int64_t>::max()};
            int64_t callbackMaxUs{0};
            int64_t callbackSumUs{0};
            uint64_t callbackCount{0};
        };

        std::mutex statsMutex;
        int64_t latencyMinUs{std::numeric_limits<int64_t>::max()};
        int64_t latencyMaxUs{0};
        int64_t latencySumUs{0};
        uint64_t latencyCount{0};
        std::vector<int64_t> latencySamplesUs{};
        int64_t callbackMinUs{std::numeric_limits<int64_t>::max()};
        int64_t callbackMaxUs{0};
        int64_t callbackSumUs{0};
        uint64_t callbackCount{0};
        int64_t spinMinUs{std::numeric_limits<int64_t>::max()};
        int64_t spinMaxUs{0};
        int64_t spinSumUs{0};
        uint64_t spinCount{0};
        uint64_t spinActiveCount{0};
        uint64_t spinIdleCount{0};
        uint64_t publishesDeduplicated{0};
        std::unordered_map<std::string, ChannelStatsAccumulator> channelStats;

        explicit Impl(const std::string& /*provider*/)
        {
            // iceoryx2 does not have a "provider URL" concept; the parameter
            // is kept for API compatibility with the old LCM signature.
            // Node creation can fail (corrupted iox2 state, permissions,
            // exceeded max nodes, …). On failure we leave `node` null and
            // every Transport entry-point short-circuits to a clean error
            // instead of aborting the whole process.
            auto built = retryIox(
                []() {
                    return iox2::NodeBuilder().create<iox2::ServiceType::Ipc>();
                },
                "NodeBuilder::create");
            if (!built.has_value())
            {
                std::cerr << "raccoon::Transport: NodeBuilder::create failed "
                             "after retries — Transport will refuse all calls\n";
                return;
            }
            node = std::make_unique<IoxNode>(std::move(built.value()));
        }

        bool initialize() const { return node != nullptr; }

        // Get-or-create a publisher port for the given channel. Returns
        // nullptr on failure (invalid channel name, iox2 service open
        // refused after retries, …). All calls into iceoryx2 must hold
        // apiMutex. NEVER aborts the process — every iox2 Expected is
        // checked, transient errors get retried via `retryIox`.
        IoxPublisher* publisherFor(const std::string& channel,
                                   [[maybe_unused]] bool retained)
        {
            auto it = publishers.find(channel);
            if (it != publishers.end()) return it->second.publisher.get();

            auto svcName = tryMakeServiceName(channel);
            if (!svcName)
            {
                std::cerr << "raccoon::Transport: invalid channel name '"
                          << channel << "' (rejected by iceoryx2)\n";
                return nullptr;
            }

            // Open or create the pub/sub service. Retries cover the
            // OpenIsMarkedForDestruction / IsBeingCreatedByAnotherInstance /
            // OpenServiceInCorruptedState transients (the third is what
            // fires on the post-reboot cleanup path).
            const std::string svcLabel =
                "publisher open_or_create('" + channel + "')";
            auto svcResult = retryIox(
                [&]() {
                    return node->service_builder(*svcName)
                        .publish_subscribe<IoxPayload>()
                        .max_publishers(8)
                        .max_subscribers(16)
                        // Always 1: cheap retain, and crucially the SAME
                        // value across every open_or_create() of a given
                        // service. Otherwise the second caller hits
                        // OpenDoesNotSupportRequestedMinHistorySize when
                        // its requested minimum is greater than the live
                        // history.
                        .history_size(1)
                        .subscriber_max_buffer_size(64)
                        .open_or_create();
                },
                svcLabel.c_str());
            if (!svcResult.has_value())
            {
                return nullptr;
            }

            PubEntry entry{};
            entry.service = std::make_unique<IoxService>(
                std::move(svcResult.value()));

            // Hint for the SHM allocator. Common case (scalar/vector
            // LCM-encoded messages) fits without reallocation; larger
            // payloads (cam_frame_t, screen_render_t) trigger
            // PowerOfTwo growth to 8K → 16K → … as needed.
            const std::string pubLabel =
                "publisher_builder('" + channel + "')";
            auto pubResult = retryIox(
                [&]() {
                    return entry.service->publisher_builder()
                        .initial_max_slice_len(kInitialSliceHint)
                        .allocation_strategy(iox2::AllocationStrategy::PowerOfTwo)
                        .create();
                },
                pubLabel.c_str());
            if (!pubResult.has_value())
            {
                return nullptr;
            }
            entry.publisher = std::make_unique<IoxPublisher>(
                std::move(pubResult.value()));

            auto* raw = entry.publisher.get();
            publishers.emplace(channel, std::move(entry));
            return raw;
        }

        // Get-or-create a subscriber entry for the channel. Multiple
        // subscribe() calls on the same channel share one iceoryx2 port and
        // fan out callbacks at dispatch time. Returns nullptr on a clean
        // failure (invalid name, iox2 open refused after retries) — NEVER
        // aborts the process.
        SubEntry* subscriberFor(const std::string& channel, bool requestRetained)
        {
            for (auto& s : subscribers)
            {
                if (s->channel == channel) return s.get();
            }

            auto svcName = tryMakeServiceName(channel);
            if (!svcName)
            {
                std::cerr << "raccoon::Transport: invalid channel name '"
                          << channel << "' (rejected by iceoryx2)\n";
                return nullptr;
            }

            const std::string svcLabel =
                "subscriber open_or_create('" + channel + "')";
            auto svcResult = retryIox(
                [&]() {
                    return node->service_builder(*svcName)
                        .publish_subscribe<IoxPayload>()
                        .max_publishers(8)
                        .max_subscribers(16)
                        .history_size(1)
                        .subscriber_max_buffer_size(64)
                        .open_or_create();
                },
                svcLabel.c_str());
            if (!svcResult.has_value())
            {
                return nullptr;
            }

            auto entry = std::make_unique<SubEntry>();
            entry->channel = channel;
            entry->retained = requestRetained;
            entry->service = std::make_unique<IoxService>(
                std::move(svcResult.value()));

            const std::string subLabel =
                "subscriber_builder('" + channel + "')";
            auto subResult = retryIox(
                [&]() {
                    return entry->service->subscriber_builder()
                        .buffer_size(64)
                        .create();
                },
                subLabel.c_str());
            if (!subResult.has_value())
            {
                return nullptr;
            }
            entry->subscriber = std::make_unique<IoxSubscriber>(
                std::move(subResult.value()));

            auto* raw = entry.get();
            subscribers.push_back(std::move(entry));
            return raw;
        }

        void recordLatency(const std::string& channel, int64_t us)
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            if (us < 0) return;
            latencyMinUs = std::min(latencyMinUs, us);
            latencyMaxUs = std::max(latencyMaxUs, us);
            latencySumUs += us;
            ++latencyCount;
            latencySamplesUs.push_back(us);
            auto& stats = channelStats[channel];
            stats.latencyMinUs = std::min(stats.latencyMinUs, us);
            stats.latencyMaxUs = std::max(stats.latencyMaxUs, us);
            stats.latencySumUs += us;
            ++stats.latencyCount;
            stats.latencySamplesUs.push_back(us);
        }

        void recordCallback(const std::string& channel, int64_t us)
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            if (us < 0) return;
            callbackMinUs = std::min(callbackMinUs, us);
            callbackMaxUs = std::max(callbackMaxUs, us);
            callbackSumUs += us;
            ++callbackCount;
            auto& stats = channelStats[channel];
            ++stats.deliveries;
            stats.callbackMinUs = std::min(stats.callbackMinUs, us);
            stats.callbackMaxUs = std::max(stats.callbackMaxUs, us);
            stats.callbackSumUs += us;
            ++stats.callbackCount;
        }

        void recordSpin(int result, int64_t us)
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            if (us >= 0)
            {
                spinMinUs = std::min(spinMinUs, us);
                spinMaxUs = std::max(spinMaxUs, us);
                spinSumUs += us;
                ++spinCount;
            }
            if (result > 0)      ++spinActiveCount;
            else if (result == 0) ++spinIdleCount;
        }

        TransportStats getAndResetStats()
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            TransportStats stats{};
            stats.publishesDeduplicated = publishesDeduplicated;
            if (latencyCount > 0)
            {
                stats.latency.minUs = latencyMinUs;
                stats.latency.maxUs = latencyMaxUs;
                stats.latency.avgUs = static_cast<int64_t>(latencySumUs / latencyCount);
                stats.latency.p99Us = percentile99(latencySamplesUs);
                stats.latency.count = latencyCount;
            }
            if (callbackCount > 0)
            {
                stats.callback.minUs = callbackMinUs;
                stats.callback.maxUs = callbackMaxUs;
                stats.callback.avgUs = static_cast<int64_t>(callbackSumUs / callbackCount);
                stats.callback.totalUs = callbackSumUs;
                stats.callback.count = callbackCount;
            }
            if (spinCount > 0)
            {
                stats.spin.minUs = spinMinUs;
                stats.spin.maxUs = spinMaxUs;
                stats.spin.avgUs = static_cast<int64_t>(spinSumUs / spinCount);
                stats.spin.count = spinCount;
            }
            stats.spin.activeCount = spinActiveCount;
            stats.spin.idleCount = spinIdleCount;

            stats.channels.reserve(channelStats.size());
            for (const auto& [channel, channelStat] : channelStats)
            {
                TransportStats::Channel entry{};
                entry.name = channel;
                entry.deliveries = channelStat.deliveries;
                if (channelStat.latencyCount > 0)
                {
                    entry.latency.minUs = channelStat.latencyMinUs;
                    entry.latency.maxUs = channelStat.latencyMaxUs;
                    entry.latency.avgUs = static_cast<int64_t>(
                        channelStat.latencySumUs / channelStat.latencyCount);
                    entry.latency.p99Us = percentile99(channelStat.latencySamplesUs);
                    entry.latency.count = channelStat.latencyCount;
                }
                if (channelStat.callbackCount > 0)
                {
                    entry.callback.minUs = channelStat.callbackMinUs;
                    entry.callback.maxUs = channelStat.callbackMaxUs;
                    entry.callback.avgUs = static_cast<int64_t>(
                        channelStat.callbackSumUs / channelStat.callbackCount);
                    entry.callback.totalUs = channelStat.callbackSumUs;
                    entry.callback.count = channelStat.callbackCount;
                }
                stats.channels.push_back(std::move(entry));
            }
            std::sort(stats.channels.begin(), stats.channels.end(),
                [](const TransportStats::Channel& a, const TransportStats::Channel& b)
                {
                    if (a.deliveries != b.deliveries) return a.deliveries > b.deliveries;
                    return a.callback.totalUs > b.callback.totalUs;
                });

            latencyMinUs = std::numeric_limits<int64_t>::max();
            latencyMaxUs = 0;
            latencySumUs = 0;
            latencyCount = 0;
            latencySamplesUs.clear();
            callbackMinUs = std::numeric_limits<int64_t>::max();
            callbackMaxUs = 0;
            callbackSumUs = 0;
            callbackCount = 0;
            spinMinUs = std::numeric_limits<int64_t>::max();
            spinMaxUs = 0;
            spinSumUs = 0;
            spinCount = 0;
            spinActiveCount = 0;
            spinIdleCount = 0;
            publishesDeduplicated = 0;
            channelStats.clear();
            return stats;
        }

        // (drainAll() removed — dispatch now lives inline in Transport::spinOnce
        // so it can drop the apiMutex before invoking handlers. Holding the
        // mutex across dispatch deadlocked against SharedTransport's mu_.)
    };

    Transport::Transport() = default;
    Transport::~Transport() = default;
    Transport::Transport(Transport&&) noexcept = default;
    Transport& Transport::operator=(Transport&&) noexcept = default;

    Transport Transport::create(const std::string& provider)
    {
        Transport t;
        try
        {
            t.impl_ = std::make_unique<Impl>(provider);
        }
        catch (const std::exception& e)
        {
            std::cerr << "raccoon::Transport: iceoryx2 init failed: "
                      << e.what() << std::endl;
        }
        if (!t.impl_ || !t.impl_->initialize())
        {
            std::cerr << "raccoon::Transport: failed to create iceoryx2 node"
                      << std::endl;
        }
        return t;
    }

    bool Transport::publishRaw(const std::string& channel, const void* data, int dataLen,
                               const PublishOptions& options)
    {
        if (!impl_ || !impl_->node) return false;
        if (dataLen < 0) return false;

        std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);

        // options.reliable is intentionally ignored — see header comment.
        (void)options.reliable;

        if (options.deduplicate)
        {
            auto& cached = impl_->deduplicationCache[channel];
            if (cached.size() == static_cast<size_t>(dataLen) &&
                std::memcmp(cached.data(), data, dataLen) == 0)
            {
                std::lock_guard<std::mutex> statsLock(impl_->statsMutex);
                ++impl_->publishesDeduplicated;
                return true;
            }
            cached.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + dataLen);
        }

        auto* pub = impl_->publisherFor(channel, options.retained);
        if (!pub) return false;

        // Loan a slice sized exactly for this message. iceoryx2 grows the
        // SHM pool transparently via PowerOfTwo when dataLen exceeds the
        // current pool's max slice; for the steady-state hot path the
        // initial 4 KB hint covers most messages without reallocation.
        //
        // loan_slice_uninit can fail (publisher disconnected, allocation
        // refused). Return false on failure instead of aborting via .value().
        auto loaned = pub->loan_slice_uninit(static_cast<uint64_t>(dataLen));
        if (!loaned.has_value()) return false;
        auto sample = std::move(loaned.value());
        std::memcpy(sample.payload_mut().data(), data, static_cast<std::size_t>(dataLen));
        auto init = iox2::assume_init(std::move(sample));
        return iox2::send(std::move(init)).has_value();
    }

    bool Transport::subscribeRaw(const std::string& channel, RawHandler handler,
                                 const SubscribeOptions& options)
    {
        if (!impl_ || !impl_->node) return false;

        std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);

        auto instrumentedHandler = [this, channel, handler = std::move(handler)]
                                   (const void* data, int dataLen)
        {
            const auto started = std::chrono::steady_clock::now();
            handler(data, dataLen);
            const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count();
            recordCallback(channel, elapsedUs);
        };

        // options.reliable is intentionally ignored — see header comment.
        (void)options.reliable;

        auto* sub = impl_->subscriberFor(channel, options.requestRetained);
        if (!sub) return false;
        sub->handlers.push_back(std::move(instrumentedHandler));
        return true;
    }

    void Transport::recordLatency(const std::string& channel, int64_t us)
    {
        if (impl_) impl_->recordLatency(channel, us);
    }

    void Transport::recordCallback(const std::string& channel, int64_t us)
    {
        if (impl_) impl_->recordCallback(channel, us);
    }

    void Transport::recordSpin(int result, int64_t us)
    {
        if (impl_) impl_->recordSpin(result, us);
    }

    TransportStats Transport::getAndResetStats()
    {
        if (!impl_) return {};
        return impl_->getAndResetStats();
    }

    int Transport::spinOnce(int timeoutMs)
    {
        if (!impl_ || !impl_->node) return -1;
        const auto started = std::chrono::steady_clock::now();
        int dispatched = 0;
        // We must NOT hold apiMutex while invoking subscriber callbacks:
        // SharedTransport's handlers acquire their own mutex (mu_), and a
        // concurrent subscribe() acquires mu_ first then tries to enter
        // Transport. Holding apiMutex through the callback deadlocked
        // every Python subscriber on the Pi. Strategy:
        //   1. Take apiMutex, snapshot subscriber pointers (raw — entries
        //      never get freed) and call iceoryx2 receive() for each.
        //   2. Drop apiMutex.
        //   3. Dispatch the snapshot lock-free.
        const auto deadline = started + std::chrono::milliseconds(std::max(0, timeoutMs));

        // Adaptive backoff between empty polls.
        //
        // Profiled on a Pi 3B (strace/py-spy native) during an active
        // mission with the reader publishing IMU/BEMF at ~80 Hz: the
        // spin thread did ~2000 clock_nanosleep/sec and burned 23 %
        // CPU just calling iceoryx2 receive() on every subscribed
        // channel (~40 of them after LcmReader's pre-subscribe pass).
        // iox2 has no blocking receive in pub/sub mode (without a
        // paired event service), so we cannot eliminate polling, but
        // the previous 200 µs fast tick was 5–10× more aggressive
        // than any consumer in raccoon-lib actually needs:
        //   sensor consumers (IMU, BEMF, button)       : ≥ 50 Hz tolerable
        //   motor mode_cmd round-trip                  : ≥ 100 Hz tolerable
        //   asyncio mission step loop                  : 10 ms granularity
        //
        // The 1 ms fast tick keeps active-traffic latency under one
        // poll cycle (well below any control loop's budget) and the
        // 10 ms idle tick collapses the cost when nothing's flowing.
        // The deadline-bounded outer loop is unchanged, so callers
        // still get the timeoutMs they asked for.
        constexpr auto kFastSleep = std::chrono::milliseconds(1);
        constexpr auto kSlowSleep = std::chrono::milliseconds(10);
        constexpr int kFastIterations = 4;
        int emptyIterations = 0;

        do
        {
            // Per-iteration drained samples. We memcpy the payload out of
            // iceoryx2's borrowed SHM into a heap buffer because the sample
            // (and its slot) is released when the loop iteration ends —
            // dispatching outside the lock can't safely touch the iceoryx2
            // memory. For sensor messages this is a few-byte copy; for
            // ~200 KB camera frames it adds ~50 µs on a Pi 3B, acceptable
            // at ~30 Hz.
            struct Pending
            {
                Impl::SubEntry*      sub;
                std::vector<uint8_t> bytes;
            };
            std::vector<Pending> pending;
            pending.reserve(impl_ ? impl_->subscribers.size() : 0);
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);
                for (auto& sub : impl_->subscribers)
                {
                    if (!sub->subscriber) continue;
                    // receive() returns Expected<Optional<Sample>, Error>.
                    // On error we skip this subscriber for this cycle —
                    // the next spinOnce will try again. Never abort via
                    // .value() on a failed Expected.
                    auto recv = sub->subscriber->receive();
                    if (!recv.has_value()) continue;
                    auto sample = std::move(recv.value());
                    if (!sample.has_value()) continue;
                    const auto& slice = sample->payload();
                    const auto n = slice.number_of_bytes();
                    if (n == 0) continue;
                    Pending p;
                    p.sub = sub.get();
                    p.bytes.resize(static_cast<std::size_t>(n));
                    std::memcpy(p.bytes.data(), slice.data(), static_cast<std::size_t>(n));
                    pending.push_back(std::move(p));
                }
            }
            // Dispatch lock-free.
            for (auto& p : pending)
            {
                const int dataLen = static_cast<int>(p.bytes.size());
                // Copy handler list locally so a subscribe inside the
                // callback doesn't invalidate our iteration.
                auto handlers = p.sub->handlers;
                for (auto& h : handlers)
                {
                    h(p.bytes.data(), dataLen);
                }
                ++dispatched;
            }
            if (timeoutMs == 0) break;

            if (pending.empty())
            {
                ++emptyIterations;
            }
            else
            {
                emptyIterations = 0;
            }
            std::this_thread::sleep_for(
                emptyIterations < kFastIterations ? kFastSleep : kSlowSleep);
        }
        while (std::chrono::steady_clock::now() < deadline);
        recordSpin(dispatched, std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count());
        return dispatched;
    }

    void Transport::spin()
    {
        if (!impl_ || !impl_->node) return;
        impl_->running = true;
        while (impl_->running)
        {
            const auto started = std::chrono::steady_clock::now();
            int dispatched = spinOnce(1);
            recordSpin(dispatched, std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started).count());
        }
    }

    void Transport::stop()
    {
        if (impl_) impl_->running = false;
    }

    void Transport::shutdown()
    {
        if (!impl_) return;
        impl_->running = false;
        // Destroy iceoryx2 ports + Node deterministically here, while the
        // caller still controls execution. After this returns, impl_ is
        // null and every entry point above short-circuits. ~Transport()
        // then no-ops on a null impl_ in the eventual static teardown.
        impl_.reset();
    }

    bool Transport::is_alive() const noexcept
    {
        return impl_ != nullptr;
    }
}
