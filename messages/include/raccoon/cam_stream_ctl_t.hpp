#pragma once

#include <cstdint>

namespace raccoon
{
    struct cam_stream_ctl_t
    {
        int64_t timestamp = 0;
        int8_t enabled = 0;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
