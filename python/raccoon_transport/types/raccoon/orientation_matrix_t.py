from __future__ import annotations

from dataclasses import dataclass, field

from ._codec import pack_f32, pack_i64, read_f32, read_i64


@dataclass
class orientation_matrix_t:
    timestamp: int = 0
    m: list[float] = field(default_factory=lambda: [0.0] * 9)

    def encode(self) -> bytes:
        return b"".join([pack_i64(self.timestamp), *(pack_f32(value) for value in self.m)])

    @classmethod
    def decode(cls, data: bytes) -> "orientation_matrix_t":
        timestamp, offset = read_i64(data, 0)
        values: list[float] = []
        for _ in range(9):
            value, offset = read_f32(data, offset)
            values.append(value)
        return cls(timestamp=timestamp, m=values)
