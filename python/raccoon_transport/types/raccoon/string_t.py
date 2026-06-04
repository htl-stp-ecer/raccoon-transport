from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class string_t:
    timestamp: int = 0
    value: str = ""

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_string(self.value)

    @classmethod
    def decode(cls, data: bytes) -> "string_t":
        timestamp, offset = read_i64(data, 0)
        value, _ = read_string(data, offset)
        return cls(timestamp=timestamp, value=value)
