#pragma once

#include <chrono>
#include <cstdint>

namespace raccoon
{
    // `reliable` delivery IS implemented on the raccoon_ring SHM backend
    // (see Transport.cpp's "Reliable delivery" section). It is NOT a no-op.
    //
    // Rationale: the SHM ring is lossless for a warm, continuously-drained
    // channel, but a SINGLE one-shot command published to a channel that is
    // cold (first-ever publish, subscriber not yet attached) or to a
    // subscriber that is briefly not draining CAN be missed with no chance
    // of self-healing — a streamed channel recovers on its next frame, a
    // fire-once command does not. This is exactly the dropped-cone-pusher
    // `motor/3/velocity_cmd` bug (run1_20260711-213952): the velocity was
    // published best-effort, lost, and the motor sat still.
    //
    // With `reliable = true` the publisher keeps re-sending the raw payload
    // every `retryInterval` until the subscriber ACKs it (or `maxRetries`
    // is hit, which is logged loudly). The wire format on the data channel
    // is unchanged, so best-effort subscribers keep working; a `reliable`
    // subscriber additionally emits an ACK and de-duplicates the re-sends.
    #define RACCOON_DEPRECATED_RELIABLE /* reliable is now functional */

    /** Per-publish transport features such as reliability, retain, and deduplication. */
    struct PublishOptions
    {
        /// At-least-once delivery: re-send until the subscriber ACKs, up to
        /// `maxRetries` attempts spaced `retryInterval` apart. Use for
        /// discrete one-shot commands (motor velocity/mode/position, servo
        /// moves). Do NOT use for high-rate streams (chassis velocity) —
        /// those self-heal on the next frame and would flood the retry queue.
        bool reliable = false;
        bool retained = false;
        /// Drop publishes whose payload (excluding the 8-byte timestamp) is
        /// byte-identical to the previous one on this channel. Honoured only
        /// for VALUE channels — command channels (Channels::isCommandChannel)
        /// are never deduplicated, so an identical re-issued command always
        /// reaches the subscriber. No-op on command channels.
        bool deduplicate = false;
        // Re-send every `retryInterval` until the subscriber ACKs. The ACK is
        // what stops retransmission; `maxRetries` is only a backstop for when
        // NO ack ever arrives (subscriber dead) so the pending queue can't grow
        // without bound. 20 × 50 ms ≈ 1 s of retrying before giving up loudly —
        // far longer than the ~ms ACK round-trip a live subscriber needs.
        std::chrono::milliseconds retryInterval{50};
        uint32_t maxRetries = 20;
    };

    /** Per-subscription features such as reliable delivery and retained replay. */
    struct SubscribeOptions
    {
        /// Emit an ACK for every reliable frame received and de-duplicate
        /// re-sends (deliver each unique command to the handler once). Must
        /// be set for the subscriber side of a `reliable` publish to stop
        /// the publisher's retransmissions.
        bool reliable = false;
        bool requestRetained = false;
    };
}
