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
        // Subscriber handlers that threw an exception. The transport keeps
        // dispatching; see Transport::setHandlerErrorHandler().
        uint64_t handlerErrors = 0;
        std::vector<Channel> channels{};
    };

    /**
     * Typed wrapper around the shared-memory transport (one ring buffer file
     * per channel under /dev/shm, see raccoon_ring.h) with reliable and
     * retained delivery.
     *
     * The implementation is hidden behind a PIMPL. Public users interact
     * through typed `publish<T>()` and `subscribe<T>()` helpers or their raw
     * equivalents when a message type is not available at compile time.
     *
     * Thread safety: every public method is safe to call from any thread,
     * with one rule: call `spinOnce()` / `spin()` from a single thread. The
     * implementation serialises access to the rings with a recursive mutex,
     * so several threads can publish concurrently (writers serialise, they
     * do not race). Subscriber callbacks run without the mutex held, so a
     * callback may publish, subscribe or call `shutdown()`, and a slow
     * callback does not block unrelated transport operations on other
     * threads.
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

        /** Reliable-delivery lifecycle events, for logging/telemetry. */
        enum class ReliableEvent
        {
            Retransmit,  ///< a reliable command was not ACKed in time, re-sent
            GaveUp       ///< maxRetries hit without an ACK — command lost
        };
        using ReliableEventHandler = std::function<void(
            ReliableEvent event, const std::string& channel, int64_t key,
            uint32_t attempts)>;
        /**
         * Register a callback fired on every reliable retransmit and give-up.
         * Lets the host (e.g. libstp) route these into its own structured log
         * (libstp.jsonl) instead of the default cerr fallback. The callback
         * runs on the transport's internal reliability thread — keep it cheap
         * and thread-safe (a log call is fine). Pass {} to clear.
         */
        void setReliableEventHandler(ReliableEventHandler handler);

        using HandlerErrorHandler = std::function<void(const std::string& channel,
                                                       const std::string& what)>;
        /**
         * Register a callback for subscriber handlers that throw. The transport
         * keeps dispatching (one faulty handler must not stop the others), counts
         * the error in `TransportStats::handlerErrors` and prints the first few per
         * channel to stderr. The callback runs on the thread that called
         * `spinOnce()`, right after the handler threw. Pass {} to clear.
         */
        void setHandlerErrorHandler(HandlerErrorHandler handler);

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

        /**
         * Deliver the frames that are waiting, then (when `timeoutMs` > 0 and
         * nothing was delivered) park until a frame arrives, `wakeControl()` is
         * called or the timeout ends. Returns the number of frames delivered.
         * With `timeoutMs == 0` the call does not block.
         */
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
         * Release all readers and writers and stop the reliability thread
         * NOW, while the host process is still fully alive. Call it from a
         * controlled shutdown path (for example a Python atexit hook) so the
         * work does not happen during static destruction. Threads inside
         * `spinOnce()` are woken, and `shutdown()` waits for them (at most one
         * second) before it closes the readers. Called from inside a
         * subscriber callback it only marks the transport as not alive; the
         * destructor releases the resources after the spin loop has returned.
         * After it returns the Transport is no longer alive — see
         * `is_alive()` — and later publish and subscribe calls are safe
         * no-ops. Replace it with another `Transport::create()` to continue.
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
