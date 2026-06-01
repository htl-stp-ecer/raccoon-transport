#pragma once

#include <cstdint>

namespace raccoon
{
    struct scalar_f_t
    {
        int64_t timestamp = 0;
        float value = 0.0f;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
