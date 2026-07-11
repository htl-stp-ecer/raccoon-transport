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
            int64_t p99Us = 0;
            uint64_t count = 0;
        };

        struct Callback
        {
            int64_t minUs = 0;
            int64_t maxUs = 0;
            int64_t avgUs = 0;
            int64_t totalUs = 0;
            uint64_t count = 0;
        };

        struct Spin
        {
            int64_t minUs = 0;
            int64_t maxUs = 0;
            int64_t avgUs = 0;
            uint64_t count = 0;
            uint64_t activeCount = 0;
            uint64_t idleCount = 0;
        };

        struct Channel
        {
            std::string name;
            uint64_t deliveries = 0;
            Latency latency{};
            Callback callback{};
        };

        Latency latency{};
        Callback callback{};
        Spin spin{};
        uint64_t publishesDeduplicated = 0;
        // Reliable-delivery visibility. `reliableRetransmits` counts every
        // re-send of an un-acked command; a non-zero value means commands
        // are being lost on first send and recovered by the retry layer.
        // `reliableDropped` counts commands that exhausted maxRetries without
        // an ACK — a genuine, unrecovered command loss worth chasing.
        uint64_t reliableRetransmits = 0;
        uint64_t reliableDropped = 0;
        std::vector<Channel> channels{};
    };

    /**
     * Typed wrapper around the shared-memory transport with optional
     * reliable and retained delivery flags preserved for API compatibility.
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

        template <TransportMessage T>
        bool publish(const std::string& channel, const T& message,
                     const PublishOptions& options = {})
        {
            // Auto-stamp the timestamp if the caller left it at the default 0.
            // Every raccoon message type carries a `timestamp` field and
            // downstream consumers dedupe by it;
            // forcing every caller to set the timestamp manually has proven
            // to be a reliable foot-gun. Non-zero values are left alone so
            // replay / test / explicit-timestamp use cases keep working.
            T stamped = message;
            if (stamped.timestamp == 0)
            {
                stamped.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }

            // Most messages are << 1 KiB (motor/servo cmds, sensor values).
            // Use a stack buffer for the common case; only heap-allocate
            // when a frame genuinely exceeds the inline capacity (camera
            // / screen render). Removes the per-publish vector malloc
            // that showed up as ~8 % of CPU in the perf trace at 200 Hz
            // motor publishes.
            constexpr int kInlineBuf = 1024;
            int maxLen = stamped.encoded_size();
            int encodedLen;
            if (maxLen <= kInlineBuf)
            {
                uint8_t inlineBuf[kInlineBuf];
                encodedLen = stamped.encode(inlineBuf, maxLen);
                if (encodedLen < 0) return false;
                return publishRaw(channel, inlineBuf, encodedLen, options);
            }
            std::vector<uint8_t> buf(maxLen);
            encodedLen = stamped.encode(buf.data(), maxLen);
            if (encodedLen < 0) return false;
            return publishRaw(channel, buf.data(), encodedLen, options);
        }

        template <TransportMessage T>
        bool subscribe(const std::string& channel,
                       std::function<void(const T &)> handler,
                       const SubscribeOptions& options = {})
        {
            return subscribeRaw(channel, [handler](const void* data, int dataLen)
            {
                // Latency stats are recorded once per frame in spinOnce's
                // raw-path drain — see Transport.cpp's readTimestampBE +
                // recordLatency call. Recording it again here double-counts.
                T msg;
                if (msg.decode(static_cast<const uint8_t*>(data), dataLen) >= 0)
                {
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

        /**
         * Wake any thread parked inside `spinOnce`. The spin thread parks
         * in a futex_waitv across every subscriber's wake_seq plus an
         * internal control word; calling `wakeControl()` bumps the
         * control word and issues a FUTEX_WAKE so the spinning thread
         * exits the wait immediately, re-snapshots its subscriber list,
         * and checks any external stop signal. Use this whenever the
         * set of subscribers changes or the spin loop needs to observe
         * an external state change without polling.
         */
        void wakeControl();

        /**
         * Tear down the iceoryx2 Node and all lazy publisher/subscriber
         * ports NOW, while the host process and iceoryx2 globals are still
         * alive. Required to avoid a static-destruction-order SIGSEGV: the
         * default `~Transport()` runs during C++ static teardown and can
         * race iceoryx2's own globals (LoggerManager, ConfigManager). Call
         * this from a Python atexit hook or your app's controlled shutdown
         * path. After this returns the Transport is no longer alive — see
         * `is_alive()` — and subsequent publish/subscribe calls become
         * safe no-ops. Re-instate by replacing it with another
         * `Transport::create()` if you actually want to keep going.
         */
        void shutdown();

        /** True once `create()` has succeeded and `shutdown()` has not run. */
        [[nodiscard]] bool is_alive() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;

        void recordLatency(const std::string& channel, int64_t us);
        void recordCallback(const std::string& channel, int64_t us);
        void recordSpin(int result, int64_t us);
    };
}
