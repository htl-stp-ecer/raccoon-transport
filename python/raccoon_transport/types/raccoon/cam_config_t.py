from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class cam_config_t:
    timestamp: int = 0
    config: str = ""

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_string(self.config)

    @classmethod
    def decode(cls, data: bytes) -> "cam_config_t":
        timestamp, offset = read_i64(data, 0)
        config, _ = read_string(data, offset)
        return cls(timestamp=timestamp, config=config)
