#pragma once

#include <cstdint>
#include <string>

namespace raccoon
{
    // A message for the user: a short title and the text below it (`messages/error`, `messages/warn`).
    struct message_t
    {
        static constexpr const char* type_name = "message_t";

        int64_t timestamp = 0;
        std::string title;
        std::string content;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
