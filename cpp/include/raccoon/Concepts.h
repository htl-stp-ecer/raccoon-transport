#pragma once

#include <concepts>
#include <cstdint>

namespace raccoon
{
    template <typename T>
    concept TransportMessage = requires(T t, const T ct, uint8_t* buf, const uint8_t* cbuf, int maxlen)
    {
        { t.timestamp } -> std::convertible_to<int64_t>;
        { ct.encoded_size() } -> std::same_as<int>;
        { ct.encode(buf, maxlen) } -> std::same_as<int>;
        { t.decode(cbuf, maxlen) } -> std::same_as<int>;
    };
}
