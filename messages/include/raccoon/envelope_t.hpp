#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace raccoon
{
    struct envelope_t
    {
        int64_t timestamp = 0;
        std::string publisher_id;
        int64_t seq_num = 0;
        std::string channel;
        int32_t payload_size = 0;
        std::vector<uint8_t> payload;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
