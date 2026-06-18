#pragma once

#include <chrono>
#include <cstdint>

namespace raccoon
{
    // The iceoryx2 backend silently ignores `reliable`, `retryInterval`,
    // and `maxRetries` — see Transport.cpp's header comment for the full
    // reasoning. In short: LCM needed at-least-once retries because UDP
    // dropped packets; iceoryx2 runs over shared memory and doesn't.
    // Keep using `retained` if you need the publisher's last sample to
    // be replayed to late subscribers (mapped to iceoryx2 history_size).
    #define RACCOON_DEPRECATED_RELIABLE \
        [[deprecated("reliable delivery is a no-op on the iceoryx2 transport — " \
                     "remove this flag; the SHM backend doesn't lose packets.")]]

    /** Per-publish transport features such as reliability, retain, and deduplication. */
    struct PublishOptions
    {
        RACCOON_DEPRECATED_RELIABLE bool reliable = false;
        bool retained = false;
        /// Drop publishes whose payload (excluding the 8-byte timestamp) is
        /// byte-identical to the previous one on this channel. Honoured only
        /// for VALUE channels — command channels (Channels::isCommandChannel)
        /// are never deduplicated, so an identical re-issued command always
        /// reaches the subscriber. No-op on command channels.
        bool deduplicate = false;
        RACCOON_DEPRECATED_RELIABLE std::chrono::milliseconds retryInterval{100};
        RACCOON_DEPRECATED_RELIABLE uint32_t maxRetries = 10;
    };

    /** Per-subscription features such as reliable delivery and retained replay. */
    struct SubscribeOptions
    {
        RACCOON_DEPRECATED_RELIABLE bool reliable = false;
        bool requestRetained = false;
    };
}
