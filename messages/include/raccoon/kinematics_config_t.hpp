#pragma once

#include <array>
#include <cstdint>

namespace raccoon
{
    struct kinematics_config_t
    {
        int64_t timestamp = 0;
        std::array<float, 12> inv_matrix{};
        std::array<float, 4> ticks_to_rad{};
        std::array<float, 12> fwd_matrix{};
        // Per-motor BEMF zero-offset (ADC counts) subtracted before integrating
        // ticks, so the position integral stays proportional to wheel angle.
        std::array<float, 4> bemf_offset{};

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
