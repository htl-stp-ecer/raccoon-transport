#include "raccoon/detail/ReliablePublisher.h"
#include "raccoon/Channels.h"
#include <raccoon/envelope_t.hpp>
#include <raccoon/ack_t.hpp>
#include <chrono>
#include <iostream>

namespace raccoon::detail
{
    ReliablePublisher::ReliablePublisher(std::string instanceId)
        : instanceId_(std::move(instanceId))
    {
    }

    bool ReliablePublisher::publish(lcm_t* lcm, const std::string& channel,
                                     const void* data, int dataLen,
                                     const PublishOptions& options)
    {
        raccoon::envelope_t env;
        env.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        env.publisher_id = instanceId_;
        env.seq_num = seqNum_++;
        env.channel = channel;
        env.payload_size = dataLen;
        env.payload.assign(static_cast<const uint8_t*>(data),
                           static_cast<const uint8_t*>(data) + dataLen);

        int maxLen = env.getEncodedSize();
        std::vector<uint8_t> buf(maxLen);
        int encodedLen = env.encode(buf.data(), 0, maxLen);
        if (encodedLen < 0) return false;

        auto reliableChannel = Channels::Protocol::reliableChannel(channel);
        int rc = lcm_publish(lcm, reliableChannel.c_str(),
                             buf.data(), static_cast<unsigned int>(encodedLen));
        if (rc != 0) return false;

        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push_back(PendingMessage{
            reliableChannel,
            std::vector<uint8_t>(buf.begin(), buf.begin() + encodedLen),
            std::chrono::steady_clock::now(),
            options.retryInterval,
            options.maxRetries,
            1,
            env.seq_num
        });

        return true;
    }

    void ReliablePublisher::startListening(lcm_t* lcm)
    {
        lcm_subscribe(lcm, Channels::Protocol::ACK, onAck, this);
    }

    void ReliablePublisher::onAck(const lcm_recv_buf_t* rbuf,
                                   const char*, void* userdata)
    {
        auto* self = static_cast<ReliablePublisher*>(userdata);

        raccoon::ack_t ack;
        if (ack.decode(rbuf->data, 0, static_cast<int>(rbuf->data_size)) < 0)
            return;

        if (ack.publisher_id != self->instanceId_)
            return;

        std::lock_guard<std::mutex> lock(self->mutex_);
        auto it = std::remove_if(self->pending_.begin(), self->pending_.end(),
            [&](const PendingMessage& msg) { return msg.seqNum == ack.seq_num; });
        self->pending_.erase(it, self->pending_.end());
    }

    void ReliablePublisher::tick(lcm_t* lcm)
    {
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mutex_);
        auto it = pending_.begin();
        while (it != pending_.end())
        {
            if (now - it->lastSent >= it->retryInterval)
            {
                if (it->attempts >= it->maxRetries)
                {
                    std::cerr << "raccoon::ReliablePublisher: max retries exhausted "
                        << "for seq=" << it->seqNum << " on " << it->reliableChannel
                        << std::endl;
                    it = pending_.erase(it);
                    continue;
                }

                lcm_publish(lcm, it->reliableChannel.c_str(),
                            it->envelopeData.data(),
                            static_cast<unsigned int>(it->envelopeData.size()));
                it->lastSent = now;
                ++it->attempts;
            }
            ++it;
        }
    }
}
