#include "raccoon/Transport.h"
#include "raccoon/detail/RetainStore.h"
#include "raccoon/Channels.h"
#include <lcm/lcm-cpp.hpp>
#include <lcm/lcm.h>
#include <raccoon/retain_request_t.hpp>
#include <iostream>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <limits>
#include <cstring>
#include <algorithm>

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

    class Transport::Impl
    {
    public:
        lcm::LCM lcm;
        std::atomic<bool> running{false};
        std::vector<std::unique_ptr<RawSubscription>> subscriptions;
        detail::RetainStore retainStore;

        // Deduplication cache
        std::unordered_map<std::string, std::vector<uint8_t>> deduplicationCache;

        // Latency stats
        std::mutex statsMutex;
        int64_t latencyMinUs{std::numeric_limits<int64_t>::max()};
        int64_t latencyMaxUs{0};
        int64_t latencySumUs{0};
        uint64_t latencyCount{0};
        uint64_t publishesDeduplicated{0};

        bool initialize(const std::string& provider)
        {
            if (provider.empty())
                lcm = lcm::LCM();
            else
                lcm = lcm::LCM(provider);

            if (!lcm.good()) return false;

            retainStore.startListening(lcm.getUnderlyingLCM());
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
        t.impl_ = std::make_unique<Impl>();
        if (!t.impl_->initialize(provider))
        {
            std::cerr << "raccoon::Transport: Failed to initialize LCM" << std::endl;
        }
        return t;
    }

    bool Transport::publishRaw(const std::string& channel, const void* data, int dataLen,
                               const PublishOptions& options)
    {
        if (!impl_ || !impl_->lcm.good()) return false;

        if (options.deduplicate)
        {
            auto& cached = impl_->deduplicationCache[channel];
            if (cached.size() == static_cast<size_t>(dataLen) &&
                std::memcmp(cached.data(), data, dataLen) == 0)
            {
                std::lock_guard<std::mutex> lock(impl_->statsMutex);
                ++impl_->publishesDeduplicated;
                return true;
            }
            cached.assign(static_cast<const uint8_t*>(data),
                          static_cast<const uint8_t*>(data) + dataLen);
        }

        if (options.reliable)
        {
            std::cerr << "raccoon::Transport: reliable not yet implemented, "
                << "falling back to plain publish on: " << channel << std::endl;
        }

        bool ok = impl_->lcm.publish(channel, data, static_cast<unsigned int>(dataLen)) == 0;

        if (ok && options.retained)
        {
            impl_->retainStore.cache(channel, data, dataLen);
        }

        return ok;
    }

    bool Transport::subscribeRaw(const std::string& channel, RawHandler handler,
                                 const SubscribeOptions& options)
    {
        if (!impl_ || !impl_->lcm.good()) return false;

        if (options.reliable)
        {
            std::cerr << "raccoon::Transport: reliable not yet implemented, "
                << "falling back to plain subscribe on: " << channel << std::endl;
        }

        auto sub = std::make_unique<RawSubscription>();
        sub->handler = std::move(handler);
        auto* subPtr = sub.get();
        impl_->subscriptions.push_back(std::move(sub));

        lcm_subscribe(impl_->lcm.getUnderlyingLCM(), channel.c_str(),
                      rawSubscribeCallback, subPtr);

        if (options.requestRetained)
        {
            raccoon::retain_request_t req{};
            req.timestamp = 0;
            req.channel = channel;
            req.subscriber_id = "";

            int maxLen = req.getEncodedSize();
            std::vector<uint8_t> buf(maxLen);
            req.encode(buf.data(), 0, maxLen);

            impl_->lcm.publish(Channels::Protocol::RETAIN_REQUEST,
                               buf.data(), static_cast<unsigned int>(maxLen));
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
        if (!impl_ || !impl_->lcm.good()) return -1;
        return impl_->lcm.handleTimeout(timeoutMs);
    }

    void Transport::spin()
    {
        if (!impl_ || !impl_->lcm.good()) return;
        impl_->running = true;
        while (impl_->running)
        {
            impl_->lcm.handleTimeout(100);
        }
    }

    void Transport::stop()
    {
        if (impl_) impl_->running = false;
    }
}