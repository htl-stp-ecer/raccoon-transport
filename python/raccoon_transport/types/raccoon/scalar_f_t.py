from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_f32, pack_i64, read_f32, read_i64


@dataclass
class scalar_f_t:
    timestamp: int = 0
    value: float = 0.0

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_f32(self.value)

    @classmethod
    def decode(cls, data: bytes) -> "scalar_f_t":
        timestamp, offset = read_i64(data, 0)
        value, _ = read_f32(data, offset)
        return cls(timestamp=timestamp, value=value)
