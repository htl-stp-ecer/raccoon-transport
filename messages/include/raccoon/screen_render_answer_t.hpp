#pragma once

#include <cstdint>
#include <string>

namespace raccoon
{
    struct screen_render_answer_t
    {
        int64_t timestamp = 0;
        std::string screen_name;
        std::string value;
        std::string reason;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
