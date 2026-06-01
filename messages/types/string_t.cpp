#include "raccoon/string_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int string_t::encoded_size() const noexcept
    {
        return 8 + codec::size_of_string(value);
    }

    int string_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp) && writer.put_string(value) ? writer.written() : -1;
    }

    int string_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp) && reader.get_string(value) ? reader.consumed() : -1;
    }
}
