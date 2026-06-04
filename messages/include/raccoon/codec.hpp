#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace raccoon::codec
{
    inline int size_of_string(std::string_view value) noexcept
    {
        return 4 + static_cast<int>(value.size());
    }

    inline int size_of_bytes(std::span<const uint8_t> value) noexcept
    {
        return 4 + static_cast<int>(value.size());
    }

    class Writer
    {
    public:
        Writer(uint8_t* data, int size) noexcept
            : data_(data), size_(size)
        {
        }

        [[nodiscard]] int written() const noexcept { return offset_; }
        [[nodiscard]] bool ok() const noexcept { return ok_; }

        bool put_i8(int8_t value) noexcept { return put_u8(static_cast<uint8_t>(value)); }

        bool put_u8(uint8_t value) noexcept
        {
            if (offset_ + 1 > size_) return fail();
            data_[offset_++] = value;
            return true;
        }

        bool put_be32(int32_t value) noexcept
        {
            return put_be32u(static_cast<uint32_t>(value));
        }

        bool put_be64(int64_t value) noexcept
        {
            return put_be64u(static_cast<uint64_t>(value));
        }

        bool put_f32(float value) noexcept
        {
            return put_be32u(std::bit_cast<uint32_t>(value));
        }

        bool put_f64(double value) noexcept
        {
            return put_be64u(std::bit_cast<uint64_t>(value));
        }

        bool put_string(std::string_view value) noexcept
        {
            if (!put_be32(static_cast<int32_t>(value.size()))) return false;
            return put_bytes(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(value.data()), value.size()));
        }

        bool put_bytes(std::span<const uint8_t> value) noexcept
        {
            if (offset_ + static_cast<int>(value.size()) > size_) return fail();
            std::memcpy(data_ + offset_, value.data(), value.size());
            offset_ += static_cast<int>(value.size());
            return true;
        }

    private:
        bool put_be32u(uint32_t value) noexcept
        {
            if (offset_ + 4 > size_) return fail();
            data_[offset_++] = static_cast<uint8_t>((value >> 24) & 0xffu);
            data_[offset_++] = static_cast<uint8_t>((value >> 16) & 0xffu);
            data_[offset_++] = static_cast<uint8_t>((value >> 8) & 0xffu);
            data_[offset_++] = static_cast<uint8_t>(value & 0xffu);
            return true;
        }

        bool put_be64u(uint64_t value) noexcept
        {
            if (offset_ + 8 > size_) return fail();
            for (int shift = 56; shift >= 0; shift -= 8)
            {
                data_[offset_++] = static_cast<uint8_t>((value >> shift) & 0xffu);
            }
            return true;
        }

        bool fail() noexcept
        {
            ok_ = false;
            return false;
        }

        uint8_t* data_;
        int size_;
        int offset_ = 0;
        bool ok_ = true;
    };

    class Reader
    {
    public:
        Reader(const uint8_t* data, int size) noexcept
            : data_(data), size_(size)
        {
        }

        [[nodiscard]] int consumed() const noexcept { return offset_; }
        [[nodiscard]] bool ok() const noexcept { return ok_; }

        bool get_i8(int8_t& value) noexcept
        {
            uint8_t raw = 0;
            if (!get_u8(raw)) return false;
            value = static_cast<int8_t>(raw);
            return true;
        }

        bool get_u8(uint8_t& value) noexcept
        {
            if (offset_ + 1 > size_) return fail();
            value = data_[offset_++];
            return true;
        }

        bool get_be32(int32_t& value) noexcept
        {
            uint32_t raw = 0;
            if (!get_be32u(raw)) return false;
            value = static_cast<int32_t>(raw);
            return true;
        }

        bool get_be64(int64_t& value) noexcept
        {
            uint64_t raw = 0;
            if (!get_be64u(raw)) return false;
            value = static_cast<int64_t>(raw);
            return true;
        }

        bool get_f32(float& value) noexcept
        {
            uint32_t raw = 0;
            if (!get_be32u(raw)) return false;
            value = std::bit_cast<float>(raw);
            return true;
        }

        bool get_f64(double& value) noexcept
        {
            uint64_t raw = 0;
            if (!get_be64u(raw)) return false;
            value = std::bit_cast<double>(raw);
            return true;
        }

        bool get_string(std::string& value) noexcept
        {
            int32_t len = 0;
            if (!get_be32(len) || len < 0 || offset_ + len > size_) return fail();
            value.assign(reinterpret_cast<const char*>(data_ + offset_), static_cast<size_t>(len));
            offset_ += len;
            return true;
        }

        bool get_bytes(std::vector<uint8_t>& value) noexcept
        {
            int32_t len = 0;
            if (!get_be32(len) || len < 0 || offset_ + len > size_) return fail();
            value.assign(data_ + offset_, data_ + offset_ + len);
            offset_ += len;
            return true;
        }

        bool take_bytes(int len, std::vector<uint8_t>& value) noexcept
        {
            if (len < 0 || offset_ + len > size_) return fail();
            value.assign(data_ + offset_, data_ + offset_ + len);
            offset_ += len;
            return true;
        }

    private:
        bool get_be32u(uint32_t& value) noexcept
        {
            if (offset_ + 4 > size_) return fail();
            value = (static_cast<uint32_t>(data_[offset_]) << 24)
                | (static_cast<uint32_t>(data_[offset_ + 1]) << 16)
                | (static_cast<uint32_t>(data_[offset_ + 2]) << 8)
                | static_cast<uint32_t>(data_[offset_ + 3]);
            offset_ += 4;
            return true;
        }

        bool get_be64u(uint64_t& value) noexcept
        {
            if (offset_ + 8 > size_) return fail();
            value = 0;
            for (int i = 0; i < 8; ++i)
            {
                value = (value << 8) | static_cast<uint64_t>(data_[offset_++]);
            }
            return true;
        }

        bool fail() noexcept
        {
            ok_ = false;
            return false;
        }

        const uint8_t* data_;
        int size_;
        int offset_ = 0;
        bool ok_ = true;
    };
}
