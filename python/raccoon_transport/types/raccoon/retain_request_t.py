from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class retain_request_t:
    timestamp: int = 0
    channel: str = ""
    subscriber_id: str = ""

    def encode(self) -> bytes:
        return b"".join(
            [pack_i64(self.timestamp), pack_string(self.channel), pack_string(self.subscriber_id)]
        )

    @classmethod
    def decode(cls, data: bytes) -> "retain_request_t":
        timestamp, offset = read_i64(data, 0)
        channel, offset = read_string(data, offset)
        subscriber_id, _ = read_string(data, offset)
        return cls(timestamp=timestamp, channel=channel, subscriber_id=subscriber_id)
