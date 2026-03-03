"""Python transport wrapper around LCM with reliable and retained delivery helpers."""

import logging
import os
import struct
import time
from collections import deque

import lcm

from .channels import ProtocolChannels
from .types.raccoon import envelope_t, ack_t

logger = logging.getLogger(__name__)


class Transport:
    """Main Python transport wrapper used by robotics applications and tests."""

    def __init__(self, provider: str = ""):
        self._lcm = lcm.LCM(provider) if provider else lcm.LCM()
        self._subscriptions = []
        self._retain_cache: dict[str, bytes] = {}

        # Reliable delivery state
        self._instance_id = os.urandom(8).hex()
        self._seq_num = 0
        self._pending: list[dict] = []
        self._dedup_ring: deque[str] = deque(maxlen=1000)
        self._dedup_set: set[str] = set()

        # Subscribe to retain requests so we can replay cached values
        self._lcm.subscribe(ProtocolChannels.RETAIN_REQUEST, self._on_retain_request)
        # Subscribe to ACKs for reliable publishing
        self._lcm.subscribe(ProtocolChannels.ACK, self._on_ack)

    @classmethod
    def create(cls, provider: str = "") -> "Transport":
        """Construct a transport, optionally using an explicit LCM provider URL."""
        return cls(provider)

    def publish(self, channel: str, message, *, reliable: bool = False, retained: bool = False,
                retry_interval_ms: int = 100, max_retries: int = 10):
        """Publish an encoded LCM message with optional reliable and retained delivery."""
        encoded = message.encode()
        if reliable:
            self._reliable_publish(channel, encoded, retry_interval_ms, max_retries)
        else:
            self._lcm.publish(channel, encoded)
        if retained:
            self._retain_cache[channel] = encoded

    def _reliable_publish(self, channel: str, data: bytes,
                          retry_interval_ms: int, max_retries: int):
        """Wrap data in envelope_t and publish on reliable channel."""
        env = envelope_t()
        env.timestamp = int(time.time() * 1e6)
        env.publisher_id = self._instance_id
        env.seq_num = self._seq_num
        self._seq_num += 1
        env.channel = channel
        env.payload_size = len(data)
        env.payload = data

        encoded = env.encode()
        reliable_channel = ProtocolChannels.reliable_channel(channel)
        self._lcm.publish(reliable_channel, encoded)

        self._pending.append({
            "reliable_channel": reliable_channel,
            "envelope_data": encoded,
            "last_sent": time.monotonic(),
            "retry_interval": retry_interval_ms / 1000.0,
            "max_retries": max_retries,
            "attempts": 1,
            "seq_num": env.seq_num,
        })

    def _on_ack(self, channel: str, data: bytes):
        """Handle incoming ACK — remove matching pending message."""
        try:
            ack = ack_t.decode(data)
        except Exception:
            return

        if ack.publisher_id != self._instance_id:
            return

        self._pending = [m for m in self._pending if m["seq_num"] != ack.seq_num]

    def _tick_reliable(self):
        """Retransmit pending messages past their retry interval."""
        if not self._pending:
            return

        now = time.monotonic()
        remaining = []
        for msg in self._pending:
            if now - msg["last_sent"] >= msg["retry_interval"]:
                if msg["attempts"] >= msg["max_retries"]:
                    logger.warning(
                        "max retries exhausted for seq=%d on %s",
                        msg["seq_num"], msg["reliable_channel"],
                    )
                    continue  # drop
                self._lcm.publish(msg["reliable_channel"], msg["envelope_data"])
                msg["last_sent"] = now
                msg["attempts"] += 1
            remaining.append(msg)
        self._pending = remaining

    def subscribe(self, channel: str, handler, *, reliable: bool = False, request_retained: bool = False):
        """Subscribe to a channel, optionally enabling reliable mode or retained replay."""
        if reliable:
            return self._reliable_subscribe(channel, handler, request_retained)

        sub = self._lcm.subscribe(channel, handler)
        self._subscriptions.append(sub)
        if request_retained:
            self._send_retain_request(channel)
        return sub

    def _reliable_subscribe(self, channel: str, handler, request_retained: bool):
        """Subscribe on the reliable envelope channel with dedup and ACK."""
        reliable_channel = ProtocolChannels.reliable_channel(channel)

        def _on_envelope(_rcv_channel: str, data: bytes):
            try:
                env = envelope_t.decode(data)
            except Exception:
                return

            if env.channel != channel:
                return

            # Send ACK
            ack = ack_t()
            ack.timestamp = int(time.time() * 1e6)
            ack.publisher_id = env.publisher_id
            ack.seq_num = env.seq_num
            ack.subscriber_id = self._instance_id
            self._lcm.publish(ProtocolChannels.ACK, ack.encode())

            # Deduplicate
            key = f"{env.publisher_id}:{env.seq_num}"
            if key in self._dedup_set:
                return

            if len(self._dedup_ring) >= 1000:
                evicted = self._dedup_ring[0]
                self._dedup_set.discard(evicted)
            self._dedup_ring.append(key)
            self._dedup_set.add(key)

            # Deliver inner payload
            handler(channel, env.payload)

        sub = self._lcm.subscribe(reliable_channel, _on_envelope)
        self._subscriptions.append(sub)
        if request_retained:
            self._send_retain_request(channel)
        return sub

    def _on_retain_request(self, channel: str, data: bytes):
        """Handle incoming retain requests by replaying cached data."""
        try:
            # Decode retain_request_t (raccoon package):
            # int64 fingerprint + int64 timestamp + string channel + string subscriber_id
            # LCM string = int32 length + bytes
            offset = 8  # skip fingerprint
            offset += 8  # skip timestamp
            chan_len = struct.unpack_from(">i", data, offset)[0]
            offset += 4
            requested_channel = data[offset:offset + chan_len].decode("utf-8")

            cached = self._retain_cache.get(requested_channel)
            if cached is not None:
                self._lcm.publish(requested_channel, cached)
        except Exception:
            logger.debug("Failed to decode retain_request_t", exc_info=True)

    def _send_retain_request(self, channel: str):
        """Send a retain_request_t for the given channel."""
        channel_bytes = channel.encode("utf-8")
        subscriber_bytes = b""
        # Layout: int64 fingerprint + int64 timestamp + string channel + string subscriber_id
        data = struct.pack(
            ">qq",
            0,  # fingerprint (not checked by C subscriber)
            0,  # timestamp
        )
        data += struct.pack(">i", len(channel_bytes)) + channel_bytes
        data += struct.pack(">i", len(subscriber_bytes)) + subscriber_bytes
        self._lcm.publish(ProtocolChannels.RETAIN_REQUEST, data)

    def spin_once(self, timeout_ms: int = 100) -> int:
        """Handle a single pending message."""
        result = self._lcm.handle_timeout(timeout_ms)
        self._tick_reliable()
        return result

    def spin(self):
        """Block and handle messages indefinitely."""
        while True:
            self._lcm.handle_timeout(100)
            self._tick_reliable()

    def close(self):
        """Unsubscribe all and clean up."""
        for sub in self._subscriptions:
            self._lcm.unsubscribe(sub)
        self._subscriptions.clear()
        self._pending.clear()
