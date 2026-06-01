from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_f32, pack_i64, read_f32, read_i64


@dataclass
class vector3f_t:
    timestamp: int = 0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0

    def encode(self) -> bytes:
        return b"".join(
            [pack_i64(self.timestamp), pack_f32(self.x), pack_f32(self.y), pack_f32(self.z)]
        )

    @classmethod
    def decode(cls, data: bytes) -> "vector3f_t":
        timestamp, offset = read_i64(data, 0)
        x, offset = read_f32(data, offset)
        y, offset = read_f32(data, offset)
        z, _ = read_f32(data, offset)
        return cls(timestamp=timestamp, x=x, y=y, z=z)
