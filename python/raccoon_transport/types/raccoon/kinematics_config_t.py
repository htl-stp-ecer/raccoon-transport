from __future__ import annotations

from dataclasses import dataclass, field

from ._codec import pack_f32, pack_i64, read_f32, read_i64


@dataclass
class kinematics_config_t:
    timestamp: int = 0
    inv_matrix: list[float] = field(default_factory=lambda: [0.0] * 12)
    ticks_to_rad: list[float] = field(default_factory=lambda: [0.0] * 4)
    fwd_matrix: list[float] = field(default_factory=lambda: [0.0] * 12)

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                *(pack_f32(value) for value in self.inv_matrix),
                *(pack_f32(value) for value in self.ticks_to_rad),
                *(pack_f32(value) for value in self.fwd_matrix),
            ]
        )

    @classmethod
    def decode(cls, data: bytes) -> "kinematics_config_t":
        timestamp, offset = read_i64(data, 0)
        inv_matrix: list[float] = []
        ticks_to_rad: list[float] = []
        fwd_matrix: list[float] = []
        for _ in range(12):
            value, offset = read_f32(data, offset)
            inv_matrix.append(value)
        for _ in range(4):
            value, offset = read_f32(data, offset)
            ticks_to_rad.append(value)
        for _ in range(12):
            value, offset = read_f32(data, offset)
            fwd_matrix.append(value)
        return cls(
            timestamp=timestamp,
            inv_matrix=inv_matrix,
            ticks_to_rad=ticks_to_rad,
            fwd_matrix=fwd_matrix,
        )
