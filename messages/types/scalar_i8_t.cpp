#include "raccoon/scalar_i8_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int scalar_i8_t::encoded_size() const noexcept
    {
        return 8 + 1;
    }

    int scalar_i8_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp) && writer.put_i8(dir) ? writer.written() : -1;
    }

    int scalar_i8_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp) && reader.get_i8(dir) ? reader.consumed() : -1;
    }
}
