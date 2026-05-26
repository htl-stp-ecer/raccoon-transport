#pragma once

#include <lcm/lcm.h>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace raccoon::detail
{
    class ReliableSubscriber
    {
    public:
        using RawHandler = std::function<void(const void* data, int dataLen)>;

        explicit ReliableSubscriber(std::string instanceId);

        void subscribe(
            lcm_t* lcm,
            const std::string& channel,
            RawHandler handler,
            std::recursive_mutex* apiMutex = nullptr);

    private:
        struct Subscription
        {
            std::string channel;
            RawHandler handler;
            ReliableSubscriber* self;
            std::recursive_mutex* apiMutex{nullptr};
        };

        static void onEnvelope(const lcm_recv_buf_t* rbuf,
                               const char* channel, void* userdata);

        bool isDuplicate(const std::string& publisherId, int64_t seqNum);

        std::string instanceId_;
        std::mutex mutex_;
        std::deque<std::string> dedupRing_;
        std::unordered_set<std::string> dedupSet_;
        std::vector<std::unique_ptr<Subscription>> subscriptions_;

        static constexpr size_t DEDUP_CAPACITY = 1000;
    };
}
