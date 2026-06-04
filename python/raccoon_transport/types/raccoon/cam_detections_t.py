from __future__ import annotations

from dataclasses import dataclass, field

from ._codec import pack_i32, pack_i64, read_i32, read_i64
from .cam_blob_t import cam_blob_t


@dataclass
class cam_detections_t:
    timestamp: int = 0
    frame_width: int = 0
    frame_height: int = 0
    num_detections: int = 0
    detections: list[cam_blob_t] = field(default_factory=list)

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                pack_i32(self.frame_width),
                pack_i32(self.frame_height),
                pack_i32(len(self.detections)),
                *(detection.encode() for detection in self.detections),
            ]
        )

    @classmethod
    def decode(cls, data: bytes) -> "cam_detections_t":
        timestamp, offset = read_i64(data, 0)
        frame_width, offset = read_i32(data, offset)
        frame_height, offset = read_i32(data, offset)
        num_detections, offset = read_i32(data, offset)
        detections: list[cam_blob_t] = []
        for _ in range(num_detections):
            detection, offset = cam_blob_t.decode_from(data, offset)
            detections.append(detection)
        return cls(
            timestamp=timestamp,
            frame_width=frame_width,
            frame_height=frame_height,
            num_detections=num_detections,
            detections=detections,
        )
