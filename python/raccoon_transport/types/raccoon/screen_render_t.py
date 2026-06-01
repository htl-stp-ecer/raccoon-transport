from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class screen_render_t:
    timestamp: int = 0
    screen_name: str = ""
    entries: str = ""

    def encode(self) -> bytes:
        return b"".join(
            [pack_i64(self.timestamp), pack_string(self.screen_name), pack_string(self.entries)]
        )

    @classmethod
    def decode(cls, data: bytes) -> "screen_render_t":
        timestamp, offset = read_i64(data, 0)
        screen_name, offset = read_string(data, offset)
        entries, _ = read_string(data, offset)
        return cls(timestamp=timestamp, screen_name=screen_name, entries=entries)
