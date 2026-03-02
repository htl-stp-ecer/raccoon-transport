#pragma once

#include "raccoon/Options.h"
#include <lcm/lcm.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace raccoon::detail
{
    class ReliablePublisher
    {
    public:
        explicit ReliablePublisher(std::string instanceId);

        bool publish(lcm_t* lcm, const std::string& channel,
                     const void* data, int dataLen,
                     const PublishOptions& options);

        void startListening(lcm_t* lcm);

        void tick(lcm_t* lcm);

    private:
        struct PendingMessage
        {
            std::string reliableChannel;
            std::vector<uint8_t> envelopeData;
            std::chrono::steady_clock::time_point lastSent;
            std::chrono::milliseconds retryInterval;
            uint32_t maxRetries;
            uint32_t attempts;
            int64_t seqNum;
        };

        static void onAck(const lcm_recv_buf_t* rbuf,
                          const char* channel, void* userdata);

        std::string instanceId_;
        int64_t seqNum_{0};
        std::mutex mutex_;
        std::vector<PendingMessage> pending_;
    };
}
