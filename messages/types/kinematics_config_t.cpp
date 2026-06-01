#include "raccoon/kinematics_config_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int kinematics_config_t::encoded_size() const noexcept
    {
        return 8 + 12 * 4 + 4 * 4 + 12 * 4;
    }

    int kinematics_config_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        if (!writer.put_be64(timestamp)) return -1;
        for (float value : inv_matrix) if (!writer.put_f32(value)) return -1;
        for (float value : ticks_to_rad) if (!writer.put_f32(value)) return -1;
        for (float value : fwd_matrix) if (!writer.put_f32(value)) return -1;
        return writer.written();
    }

    int kinematics_config_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        if (!reader.get_be64(timestamp)) return -1;
        for (float& value : inv_matrix) if (!reader.get_f32(value)) return -1;
        for (float& value : ticks_to_rad) if (!reader.get_f32(value)) return -1;
        for (float& value : fwd_matrix) if (!reader.get_f32(value)) return -1;
        return reader.consumed();
    }
}
