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
     *
     * Thread safety: every public method is safe to call from any thread.
     * The implementation serializes all underlying LCM API calls with a
     * single recursive mutex, which mirrors LCM's documented "use from one
     * thread" constraint while still letting N writers publish concurrently
     * (writers serialize, they don't race). `spin()` releases the mutex
     * between iterations so concurrent publishers see at most ~10 ms of
     * scheduling delay even when a reader thread is active on the same
     * Transport. Subscriber callbacks temporarily release the mutex while
     * user code runs, then reacquire it before returning to LCM. That keeps
     * nested publish-in-callback patterns working while preventing a slow
     * callback from blocking unrelated transport operations on other threads.
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
            // Auto-stamp the timestamp if the caller left it at the default 0.
            // Every raccoon LCM type carries a `timestamp` field (see the
            // `LcmMessage` concept) and downstream consumers dedupe by it;
            // forcing every caller to set the timestamp manually has proven
            // to be a reliable foot-gun. Non-zero values are left alone so
            // replay / test / explicit-timestamp use cases keep working.
            T stamped = message;
            if (stamped.timestamp == 0)
            {
                stamped.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }

            int maxLen = stamped.getEncodedSize();
            std::vector<uint8_t> buf(maxLen);
            int encodedLen = stamped.encode(buf.data(), 0, maxLen);
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
