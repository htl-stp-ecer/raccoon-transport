#include "raccoon/cam_stream_ctl_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int cam_stream_ctl_t::encoded_size() const noexcept
    {
        return 8 + 1;
    }

    int cam_stream_ctl_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp) && writer.put_i8(enabled) ? writer.written() : -1;
    }

    int cam_stream_ctl_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp) && reader.get_i8(enabled) ? reader.consumed() : -1;
    }
}
