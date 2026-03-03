#pragma once

#include <chrono>
#include <cstdint>

namespace raccoon
{
    /** Per-publish transport features such as reliability, retain, and deduplication. */
    struct PublishOptions
    {
        bool reliable = false;
        bool retained = false;
        bool deduplicate = false;
        std::chrono::milliseconds retryInterval{100};
        uint32_t maxRetries = 10;
    };

    /** Per-subscription features such as reliable delivery and retained replay. */
    struct SubscribeOptions
    {
        bool reliable = false;
        bool requestRetained = false;
    };
}
