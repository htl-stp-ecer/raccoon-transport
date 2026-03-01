#pragma once

#include <chrono>
#include <cstdint>

namespace raccoon
{
    struct PublishOptions
    {
        bool reliable = false;
        bool retained = false;
        bool deduplicate = false;
        std::chrono::milliseconds retryInterval{100};
        uint32_t maxRetries = 10;
    };

    struct SubscribeOptions
    {
        bool reliable = false;
        bool requestRetained = false;
    };
}