from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i32, pack_i64, read_i32, read_i64


@dataclass
class scalar_i32_t:
    timestamp: int = 0
    value: int = 0

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_i32(self.value)

    @classmethod
    def decode(cls, data: bytes) -> "scalar_i32_t":
        timestamp, offset = read_i64(data, 0)
        value, _ = read_i32(data, offset)
        return cls(timestamp=timestamp, value=value)
