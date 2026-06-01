from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class screen_render_answer_t:
    timestamp: int = 0
    screen_name: str = ""
    value: str = ""
    reason: str = ""

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                pack_string(self.screen_name),
                pack_string(self.value),
                pack_string(self.reason),
            ]
        )

    @classmethod
    def decode(cls, data: bytes) -> "screen_render_answer_t":
        timestamp, offset = read_i64(data, 0)
        screen_name, offset = read_string(data, offset)
        value, offset = read_string(data, offset)
        reason, _ = read_string(data, offset)
        return cls(timestamp=timestamp, screen_name=screen_name, value=value, reason=reason)
