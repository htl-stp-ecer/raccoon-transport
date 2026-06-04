#pragma once

#include <cstdint>

namespace raccoon
{
    struct vector3f_t
    {
        int64_t timestamp = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
