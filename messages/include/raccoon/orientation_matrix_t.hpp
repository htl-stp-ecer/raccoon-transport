#pragma once

#include <array>
#include <cstdint>

namespace raccoon
{
    struct orientation_matrix_t
    {
        int64_t timestamp = 0;
        std::array<float, 9> m{};

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
