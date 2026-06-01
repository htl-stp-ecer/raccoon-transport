#pragma once

#include <cstdint>
#include <string>

namespace raccoon
{
    struct ack_t
    {
        int64_t timestamp = 0;
        std::string publisher_id;
        int64_t seq_num = 0;
        std::string subscriber_id;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
