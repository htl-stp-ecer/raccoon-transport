from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class message_t:
    """A message for the user: a short title and the text below it."""

    timestamp: int = 0
    title: str = ""
    content: str = ""

    def encode(self) -> bytes:
        return pack_i64(self.timestamp) + pack_string(self.title) + pack_string(self.content)

    @classmethod
    def decode(cls, data: bytes) -> "message_t":
        timestamp, offset = read_i64(data, 0)
        title, offset = read_string(data, offset)
        content, _ = read_string(data, offset)
        return cls(timestamp=timestamp, title=title, content=content)
