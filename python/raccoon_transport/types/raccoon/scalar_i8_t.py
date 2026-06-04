from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i8, pack_i64, read_i8, read_i64


@dataclass
class scalar_i8_t:
    timestamp: int = 0
    dir: int = 0

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_i8(self.dir)

    @classmethod
    def decode(cls, data: bytes) -> "scalar_i8_t":
        timestamp, offset = read_i64(data, 0)
        direction, _ = read_i8(data, offset)
        return cls(timestamp=timestamp, dir=direction)
