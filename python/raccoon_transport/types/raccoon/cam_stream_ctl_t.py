from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i8, pack_i64, read_i8, read_i64


@dataclass
class cam_stream_ctl_t:
    timestamp: int = 0
    enabled: int = 0

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_i8(self.enabled)

    @classmethod
    def decode(cls, data: bytes) -> "cam_stream_ctl_t":
        timestamp, offset = read_i64(data, 0)
        enabled, _ = read_i8(data, offset)
        return cls(timestamp=timestamp, enabled=enabled)
