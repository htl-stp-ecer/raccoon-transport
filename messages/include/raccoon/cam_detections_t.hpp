#pragma once

#include "cam_blob_t.hpp"

#include <cstdint>
#include <vector>

namespace raccoon
{
    struct cam_detections_t
    {
        int64_t timestamp = 0;
        int32_t frame_width = 0;
        int32_t frame_height = 0;
        int32_t num_detections = 0;
        std::vector<cam_blob_t> detections;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
