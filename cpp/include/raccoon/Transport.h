#pragma once

#include "raccoon/Concepts.h"
#include "raccoon/Options.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace raccoon
{
    struct TransportStats
    {
        struct Latency
        {
            int64_t minUs = 0;
            int64_t maxUs = 0;
            int64_t avgUs = 0;
            uint64_t count = 0;
        };

        Latency latency{};
        uint64_t publishesDeduplicated = 0;
    };

    class Transport
    {
    public:
        Transport();
        ~Transport();

        Transport(const Transport&) = delete;
        Transport& operator=(const Transport&) = delete;
        Transport(Transport&&) noexcept;
        Transport& operator=(Transport&&) noexcept;

        static Transport create(const std::string& provider = "");

        bool publishRaw(const std::string& channel, const void* data, int dataLen,
                        const PublishOptions& options = {});

        using RawHandler = std::function<void(const void* data, int dataLen)>;
        bool subscribeRaw(const std::string& channel, RawHandler handler,
                          const SubscribeOptions& options = {});

        template <LcmMessage T>
        bool publish(const std::string& channel, const T& message,
                     const PublishOptions& options = {})
        {
            int maxLen = message.getEncodedSize();
            std::vector<uint8_t> buf(maxLen);
            int encodedLen = message.encode(buf.data(), 0, maxLen);
            if (encodedLen < 0) return false;
            return publishRaw(channel, buf.data(), encodedLen, options);
        }

        template <LcmMessage T>
        bool subscribe(const std::string& channel,
                       std::function<void(const T &)> handler,
                       const SubscribeOptions& options = {})
        {
            return subscribeRaw(channel, [this, handler](const void* data, int dataLen)
            {
                T msg;
                if (msg.decode(data, 0, dataLen) >= 0)
                {
                    auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    recordLatency(nowUs - msg.timestamp);
                    handler(msg);
                }
            }, options);
        }

        TransportStats getAndResetStats();

        int spinOnce(int timeoutMs = 0);
        void spin();
        void stop();

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;

        void recordLatency(int64_t us);
    };
}