#include "raccoon/quaternion_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int quaternion_t::encoded_size() const noexcept
    {
        return 8 + 16;
    }

    int quaternion_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp)
            && writer.put_f32(w)
            && writer.put_f32(x)
            && writer.put_f32(y)
            && writer.put_f32(z) ? writer.written() : -1;
    }

    int quaternion_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp)
            && reader.get_f32(w)
            && reader.get_f32(x)
            && reader.get_f32(y)
            && reader.get_f32(z) ? reader.consumed() : -1;
    }
}
