#pragma once

#include <cstdint>

namespace raccoon
{
    struct scalar_i32_t
    {
        int64_t timestamp = 0;
        int32_t value = 0;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
