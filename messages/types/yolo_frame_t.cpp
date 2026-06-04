#include "raccoon/yolo_frame_t.hpp"

#include "raccoon/codec.hpp"

#include <span>

namespace raccoon
{
    int yolo_frame_t::encoded_size() const noexcept
    {
        int size = 8 + 4 + 4 + 4 + static_cast<int>(frame_data.size()) + 4;
        for (const auto& box : boxes)
        {
            size += box.encoded_size();
        }
        return size;
    }

    int yolo_frame_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        if (!writer.put_be64(timestamp)
            || !writer.put_be32(frame_width)
            || !writer.put_be32(frame_height)
            || !writer.put_be32(static_cast<int32_t>(frame_data.size()))
            || !writer.put_bytes(std::span<const uint8_t>(frame_data))
            || !writer.put_be32(static_cast<int32_t>(boxes.size())))
        {
            return -1;
        }
        int offset = writer.written();
        for (const auto& box : boxes)
        {
            const int written = box.encode(buf + offset, buf_len - offset);
            if (written < 0) return -1;
            offset += written;
        }
        return offset;
    }

    int yolo_frame_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        if (!reader.get_be64(timestamp)
            || !reader.get_be32(frame_width)
            || !reader.get_be32(frame_height)
            || !reader.get_be32(frame_size)
            || frame_size < 0
            || !reader.take_bytes(frame_size, frame_data)
            || !reader.get_be32(num_boxes)
            || num_boxes < 0)
        {
            return -1;
        }
        boxes.clear();
        boxes.reserve(static_cast<size_t>(num_boxes));
        int offset = reader.consumed();
        for (int32_t i = 0; i < num_boxes; ++i)
        {
            yolo_box_t box;
            const int consumed = box.decode(buf + offset, buf_len - offset);
            if (consumed < 0) return -1;
            boxes.push_back(std::move(box));
            offset += consumed;
        }
        return offset;
    }
}
