#include "raccoon/detail/ReliableSubscriber.h"
#include "raccoon/Channels.h"
#include <raccoon/envelope_t.hpp>
#include <raccoon/ack_t.hpp>
#include <chrono>

namespace raccoon::detail
{
    ReliableSubscriber::ReliableSubscriber(std::string instanceId)
        : instanceId_(std::move(instanceId))
    {
    }

    void ReliableSubscriber::subscribe(
        lcm_t* lcm,
        const std::string& channel,
        RawHandler handler,
        std::recursive_mutex* apiMutex)
    {
        auto sub = std::make_unique<Subscription>();
        sub->channel = channel;
        sub->handler = std::move(handler);
        sub->self = this;
        sub->apiMutex = apiMutex;

        auto reliableChannel = Channels::Protocol::reliableChannel(channel);
        lcm_subscribe(lcm, reliableChannel.c_str(), onEnvelope, sub.get());
        subscriptions_.push_back(std::move(sub));
    }

    void ReliableSubscriber::onEnvelope(const lcm_recv_buf_t* rbuf,
                                         const char*, void* userdata)
    {
        auto* sub = static_cast<Subscription*>(userdata);
        auto* self = sub->self;

        raccoon::envelope_t env;
        if (env.decode(rbuf->data, 0, static_cast<int>(rbuf->data_size)) < 0)
            return;

        if (env.channel != sub->channel)
            return;

        // Send ACK
        raccoon::ack_t ack;
        ack.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ack.publisher_id = env.publisher_id;
        ack.seq_num = env.seq_num;
        ack.subscriber_id = self->instanceId_;

        int maxLen = ack.getEncodedSize();
        std::vector<uint8_t> buf(maxLen);
        int encodedLen = ack.encode(buf.data(), 0, maxLen);
        if (encodedLen > 0)
        {
            lcm_publish(rbuf->lcm, Channels::Protocol::ACK,
                        buf.data(), static_cast<unsigned int>(encodedLen));
        }

        // Deduplicate
        if (self->isDuplicate(env.publisher_id, env.seq_num))
            return;

        // User handlers run outside the transport API mutex so a slow callback
        // cannot block unrelated publish/subscribe calls from other threads.
        if (sub->apiMutex != nullptr)
        {
            sub->apiMutex->unlock();
            try
            {
                sub->handler(env.payload.data(), env.payload_size);
            }
            catch (...)
            {
                sub->apiMutex->lock();
                throw;
            }
            sub->apiMutex->lock();
            return;
        }

        sub->handler(env.payload.data(), env.payload_size);
    }

    bool ReliableSubscriber::isDuplicate(const std::string& publisherId,
                                          int64_t seqNum)
    {
        std::string key = publisherId + ":" + std::to_string(seqNum);

        std::lock_guard<std::mutex> lock(mutex_);
        if (dedupSet_.count(key))
            return true;

        if (dedupRing_.size() >= DEDUP_CAPACITY)
        {
            dedupSet_.erase(dedupRing_.front());
            dedupRing_.pop_front();
        }

        dedupRing_.push_back(key);
        dedupSet_.insert(key);
        return false;
    }
}
