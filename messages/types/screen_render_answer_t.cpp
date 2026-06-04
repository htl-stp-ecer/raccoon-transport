#include "raccoon/screen_render_answer_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int screen_render_answer_t::encoded_size() const noexcept
    {
        return 8 + codec::size_of_string(screen_name)
            + codec::size_of_string(value)
            + codec::size_of_string(reason);
    }

    int screen_render_answer_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp)
            && writer.put_string(screen_name)
            && writer.put_string(value)
            && writer.put_string(reason) ? writer.written() : -1;
    }

    int screen_render_answer_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp)
            && reader.get_string(screen_name)
            && reader.get_string(value)
            && reader.get_string(reason) ? reader.consumed() : -1;
    }
}
