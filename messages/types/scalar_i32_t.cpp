#include "raccoon/scalar_i32_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int scalar_i32_t::encoded_size() const noexcept
    {
        return 8 + 4;
    }

    int scalar_i32_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp) && writer.put_be32(value) ? writer.written() : -1;
    }

    int scalar_i32_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp) && reader.get_be32(value) ? reader.consumed() : -1;
    }
}
