from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i64, pack_string, read_i64, read_string


@dataclass
class ack_t:
    timestamp: int = 0
    publisher_id: str = ""
    seq_num: int = 0
    subscriber_id: str = ""

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                pack_string(self.publisher_id),
                pack_i64(self.seq_num),
                pack_string(self.subscriber_id),
            ]
        )

    @classmethod
    def decode(cls, data: bytes) -> "ack_t":
        timestamp, offset = read_i64(data, 0)
        publisher_id, offset = read_string(data, offset)
        seq_num, offset = read_i64(data, offset)
        subscriber_id, _ = read_string(data, offset)
        return cls(
            timestamp=timestamp,
            publisher_id=publisher_id,
            seq_num=seq_num,
            subscriber_id=subscriber_id,
        )
