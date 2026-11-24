from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i32, pack_i64, read_fixed_bytes, read_i32, read_i64


@dataclass
class cam_frame_t:
    timestamp: int = 0
    frame_id: int = 0
    frame_width: int = 0
    frame_height: int = 0
    frame_size: int = 0
    frame_data: bytes = b""

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                pack_i64(self.frame_id),
                pack_i32(self.frame_width),
                pack_i32(self.frame_height),
                pack_i32(len(self.frame_data)),
                self.frame_data,
            ]
        )

    @classmethod
    def decode(cls, data: bytes) -> "cam_frame_t":
        timestamp, offset = read_i64(data, 0)
        frame_id, offset = read_i64(data, offset)
        frame_width, offset = read_i32(data, offset)
        frame_height, offset = read_i32(data, offset)
        frame_size, offset = read_i32(data, offset)
        frame_data, offset = read_fixed_bytes(data, offset, frame_size)
        return cls(
            timestamp=timestamp,
            frame_id=frame_id,
            frame_width=frame_width,
            frame_height=frame_height,
            frame_size=frame_size,
            frame_data=frame_data,
        )
