from __future__ import annotations

from dataclasses import dataclass

from ._codec import pack_i32, pack_i64, pack_string, read_fixed_bytes, read_i32, read_i64, read_string


@dataclass
class envelope_t:
    timestamp: int = 0
    publisher_id: str = ""
    seq_num: int = 0
    channel: str = ""
    payload_size: int = 0
    payload: bytes = b""

    def encode(self) -> bytes:
        return b"".join(
            [
                pack_i64(self.timestamp),
                pack_string(self.publisher_id),
                pack_i64(self.seq_num),
                pack_string(self.channel),
                pack_i32(len(self.payload)),
                self.payload,
            ]
        )

    @classmethod
    def decode(cls, data: bytes) -> "envelope_t":
        timestamp, offset = read_i64(data, 0)
        publisher_id, offset = read_string(data, offset)
        seq_num, offset = read_i64(data, offset)
        channel, offset = read_string(data, offset)
        payload_size, offset = read_i32(data, offset)
        payload, _ = read_fixed_bytes(data, offset, payload_size)
        return cls(
            timestamp=timestamp,
            publisher_id=publisher_id,
            seq_num=seq_num,
            channel=channel,
            payload_size=payload_size,
            payload=payload,
        )
