#include "raccoon/cam_frame_t.hpp"

#include "raccoon/codec.hpp"

#include <span>

namespace raccoon
{
    int cam_frame_t::encoded_size() const noexcept
    {
        int size = 8 + 8 + 4 + 4 + 4 + static_cast<int>(frame_data.size());
        return size;
    }

    int cam_frame_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        if (!writer.put_be64(timestamp)
            || !writer.put_be64(frame_id)
            || !writer.put_be32(frame_width)
            || !writer.put_be32(frame_height)
            || !writer.put_be32(static_cast<int32_t>(frame_data.size()))
            || !writer.put_bytes(std::span<const uint8_t>(frame_data)))
        {
            return -1;
        }
        return writer.written();
    }

    int cam_frame_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        if (!reader.get_be64(timestamp)
            || !reader.get_be64(frame_id)
            || !reader.get_be32(frame_width)
            || !reader.get_be32(frame_height)
            || !reader.get_be32(frame_size)
            || frame_size < 0
            || !reader.take_bytes(frame_size, frame_data))
        {
            return -1;
        }
        return reader.consumed();
    }
}
