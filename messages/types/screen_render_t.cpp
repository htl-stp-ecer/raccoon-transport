#include "raccoon/screen_render_t.hpp"

#include "raccoon/codec.hpp"

namespace raccoon
{
    int screen_render_t::encoded_size() const noexcept
    {
        return 8 + codec::size_of_string(screen_name) + codec::size_of_string(entries);
    }

    int screen_render_t::encode(uint8_t* buf, int buf_len) const noexcept
    {
        codec::Writer writer(buf, buf_len);
        return writer.put_be64(timestamp)
            && writer.put_string(screen_name)
            && writer.put_string(entries) ? writer.written() : -1;
    }

    int screen_render_t::decode(const uint8_t* buf, int buf_len) noexcept
    {
        codec::Reader reader(buf, buf_len);
        return reader.get_be64(timestamp)
            && reader.get_string(screen_name)
            && reader.get_string(entries) ? reader.consumed() : -1;
    }
}
