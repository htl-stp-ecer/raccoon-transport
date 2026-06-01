#pragma once

#include <cstdint>
#include <string>

namespace raccoon
{
    struct yolo_box_t
    {
        int64_t timestamp = 0;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        std::string label;
        float confidence = 0.0f;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
