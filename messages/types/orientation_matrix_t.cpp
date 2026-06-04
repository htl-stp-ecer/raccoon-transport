#include "raccoon/orientation_matrix_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int orientation_matrix_t::encoded_size() const noexcept
    {
        return 8 + static_cast<int>(m.size()) * 4;
    }

    int orientation_matrix_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        if (!writer.put_be64(timestamp))
        {
            return -1;
        }
        for (float value : m)
        {
            if (!writer.put_f32(value)) return -1;
        }
        return writer.written();
    }

    int orientation_matrix_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        if (!reader.get_be64(timestamp))
        {
            return -1;
        }
        for (float& value : m)
        {
            if (!reader.get_f32(value)) return -1;
        }
        return reader.consumed();
    }
}
