#include "raccoon/envelope_t.hpp"

#include "raccoon/codec.hpp"

#include <span>

namespace raccoon
{
    int envelope_t::encoded_size() const noexcept
    {
        return 8 + codec::size_of_string(publisher_id) + 8 + codec::size_of_string(channel)
            + 4 + static_cast<int>(payload.size());
    }

    int envelope_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp)
            && writer.put_string(publisher_id)
            && writer.put_be64(seq_num)
            && writer.put_string(channel)
            && writer.put_be32(static_cast<int32_t>(payload.size()))
            && writer.put_bytes(std::span<const uint8_t>(payload)) ? writer.written() : -1;
    }

    int envelope_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp)
            && reader.get_string(publisher_id)
            && reader.get_be64(seq_num)
            && reader.get_string(channel)
            && reader.get_be32(payload_size)
            && payload_size >= 0
            && reader.take_bytes(payload_size, payload) ? reader.consumed() : -1;
    }
}
