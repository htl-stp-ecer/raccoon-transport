from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_f32, pack_i64, pack_string, read_f32, read_i64, read_string


@dataclass
class yolo_box_t:
    timestamp: int = 0
    x: float = 0.0
    y: float = 0.0
    width: float = 0.0
    height: float = 0.0
    label: str = ""
    confidence: float = 0.0

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                pack_f32(self.x),
                pack_f32(self.y),
                pack_f32(self.width),
                pack_f32(self.height),
                pack_string(self.label),
                pack_f32(self.confidence),
            ]
        )

    @classmethod
    def decode_from(cls, data: bytes, offset: int = 0) -> tuple["yolo_box_t", int]:
        timestamp, offset = read_i64(data, offset)
        x, offset = read_f32(data, offset)
        y, offset = read_f32(data, offset)
        width, offset = read_f32(data, offset)
        height, offset = read_f32(data, offset)
        label, offset = read_string(data, offset)
        confidence, offset = read_f32(data, offset)
        return (
            cls(
                timestamp=timestamp,
                x=x,
                y=y,
                width=width,
                height=height,
                label=label,
                confidence=confidence,
            ),
            offset,
        )

    @classmethod
    def decode(cls, data: bytes) -> "yolo_box_t":
        value, _ = cls.decode_from(data, 0)
        return value
