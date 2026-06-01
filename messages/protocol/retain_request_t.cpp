#include "raccoon/retain_request_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int retain_request_t::encoded_size() const noexcept
    {
        return 8 + codec::size_of_string(channel) + codec::size_of_string(subscriber_id);
    }

    int retain_request_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp)
            && writer.put_string(channel)
            && writer.put_string(subscriber_id) ? writer.written() : -1;
    }

    int retain_request_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp)
            && reader.get_string(channel)
            && reader.get_string(subscriber_id) ? reader.consumed() : -1;
    }
}
