#include "raccoon/yolo_box_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int yolo_box_t::encoded_size() const noexcept
    {
        return 8 + 4 + 4 + 4 + 4 + codec::size_of_string(label) + 4;
    }

    int yolo_box_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp)
            && writer.put_f32(x)
            && writer.put_f32(y)
            && writer.put_f32(width)
            && writer.put_f32(height)
            && writer.put_string(label)
            && writer.put_f32(confidence) ? writer.written() : -1;
    }

    int yolo_box_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp)
            && reader.get_f32(x)
            && reader.get_f32(y)
            && reader.get_f32(width)
            && reader.get_f32(height)
            && reader.get_string(label)
            && reader.get_f32(confidence) ? reader.consumed() : -1;
    }
}
