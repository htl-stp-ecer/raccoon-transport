#pragma once

#include <cstdint>
#include <string>

namespace raccoon
{
    struct cam_config_t
    {
        int64_t timestamp = 0;
        std::string config;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
