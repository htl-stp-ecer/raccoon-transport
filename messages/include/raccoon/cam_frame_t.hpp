#pragma once

#include <cstdint>
#include <vector>

namespace raccoon
{
    struct cam_frame_t
    {
        static constexpr const char* type_name = "cam_frame_t";

        int64_t timestamp = 0;
        int64_t frame_id = 0;
        int32_t frame_width = 0;
        int32_t frame_height = 0;
        int32_t frame_size = 0;
        std::vector<uint8_t> frame_data;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
