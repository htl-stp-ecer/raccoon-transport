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
    /** Lightweight counters collected by the C++ transport implementation. */
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

    /**
     * Typed wrapper around LCM with optional reliable and retained delivery.
     *
     * The implementation is hidden behind a PIMPL. Public users interact
     * through typed `publish<T>()` and `subscribe<T>()` helpers or their raw
     * equivalents when a message type is not available at compile time.
     */
    class Transport
    {
    public:
        Transport();
        ~Transport();

        Transport(const Transport&) = delete;
        Transport& operator=(const Transport&) = delete;
        Transport(Transport&&) noexcept;
        Transport& operator=(Transport&&) noexcept;

        /** Construct and connect a transport instance, optionally using an explicit provider URL. */
        static Transport create(const std::string& provider = "");

        /** Publish a raw payload on a channel using the requested delivery options. */
        bool publishRaw(const std::string& channel, const void* data, int dataLen,
                        const PublishOptions& options = {});

        using RawHandler = std::function<void(const void* data, int dataLen)>;
        /** Subscribe to raw payload bytes on a channel. */
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

        /** Return collected stats and clear the internal counters. */
        TransportStats getAndResetStats();

        /** Handle one LCM iteration and advance reliability timers. */
        int spinOnce(int timeoutMs = 0);
        /** Block and keep handling messages until `stop()` is called. */
        void spin();
        /** Request termination of a running `spin()` loop. */
        void stop();

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;

        void recordLatency(int64_t us);
    };
}
