#pragma once

#include "yolo_box_t.hpp"

#include <cstdint>
#include <vector>

namespace raccoon
{
    struct yolo_frame_t
    {
        int64_t timestamp = 0;
        int32_t frame_width = 0;
        int32_t frame_height = 0;
        int32_t frame_size = 0;
        std::vector<uint8_t> frame_data;
        int32_t num_boxes = 0;
        std::vector<yolo_box_t> boxes;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
