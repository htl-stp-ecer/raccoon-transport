from __future__ import annotations

import struct


def pack_i8(value: int) -> bytes:
    return struct.pack(">b", value)


def pack_i32(value: int) -> bytes:
    return struct.pack(">i", value)


def pack_i64(value: int) -> bytes:
    return struct.pack(">q", value)


def pack_f32(value: float) -> bytes:
    return struct.pack(">f", value)


def pack_string(value: str) -> bytes:
    encoded = value.encode("utf-8")
    return pack_i32(len(encoded)) + encoded


def read_i8(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from(">b", data, offset)[0], offset + 1


def read_i32(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from(">i", data, offset)[0], offset + 4


def read_i64(data: bytes, offset: int) -> tuple[int, int]:
    return struct.unpack_from(">q", data, offset)[0], offset + 8


def read_f32(data: bytes, offset: int) -> tuple[float, int]:
    return struct.unpack_from(">f", data, offset)[0], offset + 4


def read_string(data: bytes, offset: int) -> tuple[str, int]:
    length, offset = read_i32(data, offset)
    end = offset + length
    return data[offset:end].decode("utf-8"), end


def read_fixed_bytes(data: bytes, offset: int, length: int) -> tuple[bytes, int]:
    end = offset + length
    return data[offset:end], end
