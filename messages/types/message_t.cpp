#include "raccoon/message_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int message_t::encoded_size() const noexcept
    {
        return 8 + codec::size_of_string(title) + codec::size_of_string(content);
    }

    int message_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp) && writer.put_string(title) && writer.put_string(content)
            ? writer.written() : -1;
    }

    int message_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp) && reader.get_string(title) && reader.get_string(content)
            ? reader.consumed() : -1;
    }
}
