#pragma once

#include <cstdint>
#include <string>

namespace raccoon
{
    struct cam_blob_t
    {
        static constexpr const char* type_name = "cam_blob_t";

        int64_t timestamp = 0;
        std::string label;
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float area = 0.0f;
        float confidence = 0.0f;
        /// ARGB colour for drawing the detection, 0 = the UI's default. The alpha byte is 0xFF when set.
        int32_t color = 0;

        [[nodiscard]] int encoded_size() const noexcept;
        int encode(uint8_t* buf, int buf_len) const noexcept;
        int decode(const uint8_t* buf, int buf_len) noexcept;
    };
}
