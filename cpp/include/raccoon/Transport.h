#pragma once

#include "raccoon/Concepts.h"
#include "raccoon/Options.h"
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace raccoon
{
    /** Outcome of a publish. */
    enum class PublishResult
    {
        Ok,            ///< the frame is in the ring
        NotInTable,    ///< the channel is not in the channel table
        TypeMismatch,  ///< the message type or the frame size does not fit the channel
        TooBig,        ///< the frame is larger than the limit of the channel
        Closed         ///< the transport was shut down, or the ring could not be opened
    };

    /** Short name of a result, for log lines. */
    [[nodiscard]] const char* to_string(PublishResult result) noexcept;

    /** Lightweight counters collected by the C++ transport implementation. */
    struct TransportStats
    {
        struct Latency
        {
            int64_t min_us = 0;
            int64_t max_us = 0;
            int64_t avg_us = 0;
            int64_t p99_us = 0;
            uint64_t count = 0;
        };

        struct Callback
        {
            int64_t min_us = 0;
            int64_t max_us = 0;
            int64_t avg_us = 0;
            int64_t total_us = 0;
            uint64_t count = 0;
        };

        struct Spin
        {
            int64_t min_us = 0;
            int64_t max_us = 0;
            int64_t avg_us = 0;
            uint64_t count = 0;
            uint64_t active_count = 0;
            uint64_t idle_count = 0;
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
        uint64_t publishes_deduplicated = 0;
        // Reliable-delivery visibility. `reliable_retransmits` counts every
        // re-send of an un-acked command; a non-zero value means commands
        // are being lost on first send and recovered by the retry layer.
        // `reliable_dropped` counts commands that exhausted max_retries without
        // an ACK — a genuine, unrecovered command loss worth chasing.
        uint64_t reliable_retransmits = 0;
        uint64_t reliable_dropped = 0;
        // Subscriber handlers that threw an exception. The transport keeps
        // dispatching; see Transport::set_handler_error_handler().
        uint64_t handler_errors = 0;
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
     * with one rule: call `spin_once()` / `spin()` from a single thread. The
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

        /**
         * Publish a raw payload. The channel must be in the channel table; a channel of a
         * fixed-size type takes exactly that size, any other channel up to its limit.
         */
        PublishResult publish_raw(std::string_view channel, const void* data, size_t len,
                                  const PublishOptions& options = {});

        /**
         * Like `publish_raw`, and checks that the channel carries the message type
         * `type_name` (`TypeMismatch` otherwise). `publish<T>()` calls this.
         */
        PublishResult publish_typed(std::string_view channel, const char* type_name,
                                    const void* data, size_t len,
                                    const PublishOptions& options = {});

        using RawHandler = std::function<void(const void* data, int dataLen)>;
        /**
         * Subscribe to raw payload bytes on a channel of the channel table. Returns false
         * if the channel is not in the table or the transport is closed. A new subscriber
         * starts after the latest frame; on a retained channel it first receives the latest
         * frame.
         */
        bool subscribe_raw(std::string_view channel, RawHandler handler);

        /** Like `subscribe_raw`, and fails if the channel does not carry `type_name`. */
        bool subscribe_typed(std::string_view channel, const char* type_name,
                             RawHandler handler);

        /** Reliable-delivery lifecycle events, for logging/telemetry. */
        enum class ReliableEvent
        {
            Retransmit,  ///< a reliable command was not ACKed in time, re-sent
            GaveUp       ///< max_retries hit without an ACK — command lost
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
        void set_reliable_event_handler(ReliableEventHandler handler);

        using HandlerErrorHandler = std::function<void(const std::string& channel,
                                                       const std::string& what)>;
        /**
         * Register a callback for subscriber handlers that throw. The transport
         * keeps dispatching (one faulty handler must not stop the others), counts
         * the error in `TransportStats::handler_errors` and prints the first few per
         * channel to stderr. The callback runs on the thread that called
         * `spin_once()`, right after the handler threw. Pass {} to clear.
         */
        void set_handler_error_handler(HandlerErrorHandler handler);

        template <TransportMessage T>
        PublishResult publish(std::string_view channel, const T& message,
                              const PublishOptions& options = {})
        {
            // Stamp the timestamp if the caller left it at 0; every message type
            // starts with it and receivers use it for latency and acknowledgements.
            T stamped = message;
            if (stamped.timestamp == 0)
            {
                stamped.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
            }

            // Most messages are small: encode into a stack buffer and only use the
            // heap for big frames (camera, screen).
            constexpr int kInlineBuf = 1024;
            const int max_len = stamped.encoded_size();
            if (max_len <= kInlineBuf)
            {
                uint8_t inline_buf[kInlineBuf];
                const int n = stamped.encode(inline_buf, max_len);
                if (n < 0) return PublishResult::TooBig;
                return publish_typed(channel, T::type_name, inline_buf, static_cast<size_t>(n), options);
            }
            std::vector<uint8_t> buf(static_cast<size_t>(max_len));
            const int n = stamped.encode(buf.data(), max_len);
            if (n < 0) return PublishResult::TooBig;
            return publish_typed(channel, T::type_name, buf.data(), static_cast<size_t>(n), options);
        }

        template <TransportMessage T>
        bool subscribe(std::string_view channel,
                       std::function<void(const T &)> handler)
        {
            return subscribe_typed(channel, T::type_name, [handler](const void* data, int len)
            {
                T msg;
                if (msg.decode(static_cast<const uint8_t*>(data), len) >= 0)
                {
                    handler(msg);
                }
            });
        }

        /** Return collected stats and clear the internal counters. */
        TransportStats get_and_reset_stats();

        /**
         * Deliver the frames that are waiting, then (when `timeout_ms` > 0 and
         * nothing was delivered) park until a frame arrives, `wake_control()` is
         * called or the timeout ends. Returns the number of frames delivered.
         * With `timeout_ms == 0` the call does not block.
         */
        int spin_once(int timeout_ms = 0);
        /** Block and keep handling messages until `stop()` is called. */
        void spin();
        /** Request termination of a running `spin()` loop. */
        void stop();

        /**
         * Wake any thread parked inside `spin_once`. The spin thread parks
         * in a futex_waitv across every subscriber's wake_seq plus an
         * internal control word; calling `wake_control()` bumps the
         * control word and issues a FUTEX_WAKE so the spinning thread
         * exits the wait immediately, re-snapshots its subscriber list,
         * and checks any external stop signal. Use this whenever the
         * set of subscribers changes or the spin loop needs to observe
         * an external state change without polling.
         */
        void wake_control();

        /**
         * Release all readers and writers and stop the reliability thread
         * NOW, while the host process is still fully alive. Call it from a
         * controlled shutdown path (for example a Python atexit hook) so the
         * work does not happen during static destruction. Threads inside
         * `spin_once()` are woken, and `shutdown()` waits for them (at most one
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
