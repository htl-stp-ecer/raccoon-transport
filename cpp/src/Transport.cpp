#include "raccoon/Transport.h"
#include "raccoon/detail/RetainStore.h"
#include "raccoon/detail/ReliablePublisher.h"
#include "raccoon/detail/ReliableSubscriber.h"
#include "raccoon/Channels.h"
#include <lcm/lcm-cpp.hpp>
#include <lcm/lcm.h>
#include <iostream>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <limits>
#include <cstring>
#include <algorithm>
#include <random>

namespace raccoon
{
    // Raw subscribe callback - forwarded from C API to stored std::function
    struct RawSubscription
    {
        Transport::RawHandler handler;
    };

    static void rawSubscribeCallback(const lcm_recv_buf_t* rbuf, const char*,
                                     void* userdata)
    {
        auto* sub = static_cast<RawSubscription*>(userdata);
        sub->handler(rbuf->data, static_cast<int>(rbuf->data_size));
    }

    static std::string generateInstanceId()
    {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        std::uniform_int_distribution<uint64_t> dist;
        uint64_t val = dist(gen);
        char buf[17];
        std::snprintf(buf, sizeof(buf), "%016lx", static_cast<unsigned long>(val));
        return std::string(buf, 16);
    }

    class Transport::Impl
    {
    public:
        // lcm::LCM has no move ctor/assignment; use unique_ptr to avoid
        // use-after-free from copy assignment.
        std::unique_ptr<lcm::LCM> lcm;
        std::atomic<bool> running{false};
        std::vector<std::unique_ptr<RawSubscription>> subscriptions;
        detail::RetainStore retainStore;

        // Reliable delivery
        std::string instanceId;
        detail::ReliablePublisher reliablePublisher;
        detail::ReliableSubscriber reliableSubscriber;

        // Deduplication cache
        std::unordered_map<std::string, std::vector<uint8_t>> deduplicationCache;

        // Serializes every call into the LCM API. lcm::LCM is documented as
        // single-threaded — concurrent publish from N writers (heartbeat
        // daemon + motor commands + servo commands + ...) without this guard
        // can corrupt the UDP send buffer, miss messages, or trip the
        // hardware watchdog because heartbeat publishes get dropped.
        //
        // Recursive so a subscriber callback that publishes back (a common
        // pattern for ack/handshake protocols) does not self-deadlock.
        // ``spinOnce``/``spin`` also take this lock, so subscriber callbacks
        // are dispatched with the mutex held — that's intentional: any
        // re-entry must go through the recursive path.
        std::recursive_mutex apiMutex;

        // Latency stats
        std::mutex statsMutex;
        int64_t latencyMinUs{std::numeric_limits<int64_t>::max()};
        int64_t latencyMaxUs{0};
        int64_t latencySumUs{0};
        uint64_t latencyCount{0};
        uint64_t publishesDeduplicated{0};

        explicit Impl(const std::string& provider = "")
            : lcm(std::make_unique<lcm::LCM>(provider))
            , instanceId(generateInstanceId())
            , reliablePublisher(instanceId)
            , reliableSubscriber(instanceId)
        {
        }

        bool initialize()
        {
            if (!lcm || !lcm->good()) return false;

            retainStore.startListening(lcm->getUnderlyingLCM());
            reliablePublisher.startListening(lcm->getUnderlyingLCM());
            return true;
        }

        void recordLatency(int64_t us)
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            if (us < 0) return; // clock skew, ignore
            latencyMinUs = std::min(latencyMinUs, us);
            latencyMaxUs = std::max(latencyMaxUs, us);
            latencySumUs += us;
            ++latencyCount;
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
                stats.latency.count = latencyCount;
            }
            // Reset
            latencyMinUs = std::numeric_limits<int64_t>::max();
            latencyMaxUs = 0;
            latencySumUs = 0;
            latencyCount = 0;
            publishesDeduplicated = 0;
            return stats;
        }
    };

    Transport::Transport() = default;
    Transport::~Transport() = default;
    Transport::Transport(Transport&&) noexcept = default;
    Transport& Transport::operator=(Transport&&) noexcept = default;

    Transport Transport::create(const std::string& provider)
    {
        Transport t;
        t.impl_ = std::make_unique<Impl>(provider);
        if (!t.impl_->initialize())
        {
            std::cerr << "raccoon::Transport: Failed to initialize LCM" << std::endl;
        }
        return t;
    }

    bool Transport::publishRaw(const std::string& channel, const void* data, int dataLen,
                               const PublishOptions& options)
    {
        if (!impl_ || !impl_->lcm || !impl_->lcm->good()) return false;

        std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);

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

        if (options.reliable)
        {
            bool ok = impl_->reliablePublisher.publish(
                impl_->lcm->getUnderlyingLCM(), channel, data, dataLen, options);
            if (ok && options.retained)
            {
                impl_->retainStore.cache(channel, data, dataLen);
            }
            return ok;
        }

        bool ok = impl_->lcm->publish(channel, data, static_cast<unsigned int>(dataLen)) == 0;

        if (ok && options.retained)
        {
            impl_->retainStore.cache(channel, data, dataLen);
        }

        return ok;
    }

    bool Transport::subscribeRaw(const std::string& channel, RawHandler handler,
                                 const SubscribeOptions& options)
    {
        if (!impl_ || !impl_->lcm || !impl_->lcm->good()) return false;

        std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);

        if (options.reliable)
        {
            impl_->reliableSubscriber.subscribe(
                impl_->lcm->getUnderlyingLCM(), channel, std::move(handler));

            if (options.requestRetained)
            {
                // Manual encoding matching Python/Dart protocol:
                // int64 fingerprint(0) + int64 timestamp(0) +
                // int32 channel_len + channel_bytes + int32 subscriber_id_len(0)
                auto chanLen = static_cast<int32_t>(channel.size());
                int totalLen = 8 + 8 + 4 + chanLen + 4;
                std::vector<uint8_t> buf(totalLen, 0);
                int off = 16; // skip fingerprint(0) + timestamp(0)
                buf[off++] = static_cast<uint8_t>((chanLen >> 24) & 0xFF);
                buf[off++] = static_cast<uint8_t>((chanLen >> 16) & 0xFF);
                buf[off++] = static_cast<uint8_t>((chanLen >>  8) & 0xFF);
                buf[off++] = static_cast<uint8_t>((chanLen      ) & 0xFF);
                std::memcpy(buf.data() + off, channel.data(), chanLen);

                impl_->lcm->publish(Channels::Protocol::RETAIN_REQUEST,
                                    buf.data(), static_cast<unsigned int>(totalLen));
            }

            return true;
        }

        auto sub = std::make_unique<RawSubscription>();
        sub->handler = std::move(handler);
        auto* subPtr = sub.get();
        impl_->subscriptions.push_back(std::move(sub));

        lcm_subscribe(impl_->lcm->getUnderlyingLCM(), channel.c_str(),
                      rawSubscribeCallback, subPtr);

        if (options.requestRetained)
        {
            // Manual encoding matching Python/Dart protocol:
            // int64 fingerprint(0) + int64 timestamp(0) +
            // int32 channel_len + channel_bytes + int32 subscriber_id_len(0)
            auto chanLen = static_cast<int32_t>(channel.size());
            int totalLen = 8 + 8 + 4 + chanLen + 4;
            std::vector<uint8_t> buf(totalLen, 0);
            int off = 16; // skip fingerprint(0) + timestamp(0)
            buf[off++] = static_cast<uint8_t>((chanLen >> 24) & 0xFF);
            buf[off++] = static_cast<uint8_t>((chanLen >> 16) & 0xFF);
            buf[off++] = static_cast<uint8_t>((chanLen >>  8) & 0xFF);
            buf[off++] = static_cast<uint8_t>((chanLen      ) & 0xFF);
            std::memcpy(buf.data() + off, channel.data(), chanLen);

            impl_->lcm->publish(Channels::Protocol::RETAIN_REQUEST,
                                buf.data(), static_cast<unsigned int>(totalLen));
        }

        return true;
    }

    void Transport::recordLatency(int64_t us)
    {
        if (impl_) impl_->recordLatency(us);
    }

    TransportStats Transport::getAndResetStats()
    {
        if (!impl_) return {};
        return impl_->getAndResetStats();
    }

    int Transport::spinOnce(int timeoutMs)
    {
        if (!impl_ || !impl_->lcm || !impl_->lcm->good()) return -1;
        std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);
        int result = impl_->lcm->handleTimeout(timeoutMs);
        impl_->reliablePublisher.tick(impl_->lcm->getUnderlyingLCM());
        return result;
    }

    void Transport::spin()
    {
        if (!impl_ || !impl_->lcm || !impl_->lcm->good()) return;
        impl_->running = true;
        // Each iteration grabs+releases the lock so a long-running spin does
        // not block concurrent publishers indefinitely. The 10 ms slice keeps
        // worst-case publish latency low enough for a 100 ms heartbeat.
        while (impl_->running)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(impl_->apiMutex);
                impl_->lcm->handleTimeout(10);
                impl_->reliablePublisher.tick(impl_->lcm->getUnderlyingLCM());
            }
        }
    }

    void Transport::stop()
    {
        if (impl_) impl_->running = false;
    }
}
