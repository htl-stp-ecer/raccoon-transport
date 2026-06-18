"""Python transport wrapper with reliable and retained delivery helpers."""

from __future__ import annotations

import logging
import os
import threading
import time
from collections import defaultdict, deque
from dataclasses import dataclass

from .channels import Channels, ProtocolChannels
from .types.raccoon import ack_t, envelope_t, retain_request_t

logger = logging.getLogger(__name__)


@dataclass
class _Subscription:
    channel: str
    callback: object
    active: bool = True


class _Broker:
    def __init__(self):
        self.subscriptions: dict[str, list[_Subscription]] = defaultdict(list)


_BROKERS: dict[str, _Broker] = {}
_BROKER_LOCK = threading.Lock()


class _Memq:
    def __init__(self, provider: str):
        key = provider or "memq://default"
        with _BROKER_LOCK:
            self._broker = _BROKERS.setdefault(key, _Broker())
        self._queue: deque[tuple[_Subscription, str, bytes]] = deque()
        self._lock = threading.Lock()

    def publish(self, channel: str, data: bytes):
        with self._lock:
            for sub in list(self._broker.subscriptions.get(channel, [])):
                if sub.active:
                    self._queue.append((sub, channel, bytes(data)))

    def subscribe(self, channel: str, callback):
        sub = _Subscription(channel=channel, callback=callback)
        with self._lock:
            self._broker.subscriptions[channel].append(sub)
        return sub

    def unsubscribe(self, sub: _Subscription):
        sub.active = False
        with self._lock:
            subs = self._broker.subscriptions.get(sub.channel, [])
            self._broker.subscriptions[sub.channel] = [item for item in subs if item is not sub]

    def handle_timeout(self, timeout_ms: int = 0) -> int:
        deadline = time.monotonic() + timeout_ms / 1000.0
        while True:
            with self._lock:
                if self._queue:
                    sub, channel, data = self._queue.popleft()
                    if sub.active:
                        sub.callback(channel, data)
                        return 1
            if timeout_ms <= 0 or time.monotonic() >= deadline:
                return 0
            time.sleep(0.001)


class Transport:
    """Main Python transport wrapper used by robotics applications and tests."""

    def __init__(self, provider: str = ""):
        self._lcm = _Memq(provider)
        self._subscriptions = []
        self._retain_cache: dict[str, bytes] = {}

        self._instance_id = os.urandom(8).hex()
        self._seq_num = 0
        self._pending: list[dict] = []
        self._dedup_ring: deque[str] = deque(maxlen=1000)
        self._dedup_set: set[str] = set()
        # Last value bytes published with deduplicate=True per channel,
        # stored WITHOUT the leading 8-byte timestamp (see publish()).
        self._last_value: dict[str, bytes] = {}

        self._lcm.subscribe(ProtocolChannels.RETAIN_REQUEST, self._on_retain_request)
        self._lcm.subscribe(ProtocolChannels.ACK, self._on_ack)

    @classmethod
    def create(cls, provider: str = "") -> "Transport":
        return cls(provider)

    def publish(
        self,
        channel: str,
        message,
        *,
        reliable: bool = False,
        retained: bool = False,
        deduplicate: bool = False,
        retry_interval_ms: int = 100,
        max_retries: int = 10,
    ):
        try:
            if getattr(message, "timestamp", None) == 0:
                message.timestamp = int(time.time() * 1e6)
        except AttributeError:
            pass

        encoded = message.encode()

        # Deduplication: drop byte-identical VALUE-channel payloads. Command
        # channels are never deduplicated (Channels.is_command_channel). The
        # comparison skips the leading 8-byte timestamp every raccoon value
        # type carries; mirrors raccoon::dedup in the C++ transport.
        if deduplicate and not Channels.is_command_channel(channel):
            key = encoded[8:] if len(encoded) > 8 else encoded
            if self._last_value.get(channel) == key:
                return
            self._last_value[channel] = key

        if reliable:
            self._reliable_publish(channel, encoded, retry_interval_ms, max_retries)
        else:
            self._lcm.publish(channel, encoded)
        if retained:
            self._retain_cache[channel] = encoded

    def _reliable_publish(
        self, channel: str, data: bytes, retry_interval_ms: int, max_retries: int
    ):
        env = envelope_t(
            timestamp=int(time.time() * 1e6),
            publisher_id=self._instance_id,
            seq_num=self._seq_num,
            channel=channel,
            payload_size=len(data),
            payload=data,
        )
        self._seq_num += 1

        encoded = env.encode()
        reliable_channel = ProtocolChannels.reliable_channel(channel)
        self._lcm.publish(reliable_channel, encoded)

        self._pending.append(
            {
                "reliable_channel": reliable_channel,
                "envelope_data": encoded,
                "last_sent": time.monotonic(),
                "retry_interval": retry_interval_ms / 1000.0,
                "max_retries": max_retries,
                "attempts": 1,
                "seq_num": env.seq_num,
            }
        )

    def _on_ack(self, channel: str, data: bytes):
        del channel
        try:
            ack = ack_t.decode(data)
        except Exception:
            return
        if ack.publisher_id != self._instance_id:
            return
        self._pending = [m for m in self._pending if m["seq_num"] != ack.seq_num]

    def _tick_reliable(self):
        if not self._pending:
            return

        now = time.monotonic()
        remaining = []
        for msg in self._pending:
            if now - msg["last_sent"] >= msg["retry_interval"]:
                if msg["attempts"] >= msg["max_retries"]:
                    logger.warning(
                        "max retries exhausted for seq=%d on %s",
                        msg["seq_num"],
                        msg["reliable_channel"],
                    )
                    continue
                self._lcm.publish(msg["reliable_channel"], msg["envelope_data"])
                msg["last_sent"] = now
                msg["attempts"] += 1
            remaining.append(msg)
        self._pending = remaining

    def subscribe(
        self, channel: str, handler, *, reliable: bool = False, request_retained: bool = False
    ):
        if reliable:
            return self._reliable_subscribe(channel, handler, request_retained)

        sub = self._lcm.subscribe(channel, handler)
        self._subscriptions.append(sub)
        if request_retained:
            self._send_retain_request(channel)
        return sub

    def _reliable_subscribe(self, channel: str, handler, request_retained: bool):
        reliable_channel = ProtocolChannels.reliable_channel(channel)

        def _on_envelope(_rcv_channel: str, data: bytes):
            try:
                env = envelope_t.decode(data)
            except Exception:
                return
            if env.channel != channel:
                return

            self._lcm.publish(
                ProtocolChannels.ACK,
                ack_t(
                    timestamp=int(time.time() * 1e6),
                    publisher_id=env.publisher_id,
                    seq_num=env.seq_num,
                    subscriber_id=self._instance_id,
                ).encode(),
            )

            key = f"{env.publisher_id}:{env.seq_num}"
            if key in self._dedup_set:
                return
            if len(self._dedup_ring) >= 1000:
                self._dedup_set.discard(self._dedup_ring[0])
            self._dedup_ring.append(key)
            self._dedup_set.add(key)
            handler(channel, env.payload)

        sub = self._lcm.subscribe(reliable_channel, _on_envelope)
        self._subscriptions.append(sub)
        if request_retained:
            self._send_retain_request(channel)
        return sub

    def _on_retain_request(self, channel: str, data: bytes):
        del channel
        try:
            request = retain_request_t.decode(data)
            cached = self._retain_cache.get(request.channel)
            if cached is not None:
                self._lcm.publish(request.channel, cached)
        except Exception:
            logger.debug("Failed to decode retain_request_t", exc_info=True)

    def _send_retain_request(self, channel: str):
        self._lcm.publish(
            ProtocolChannels.RETAIN_REQUEST,
            retain_request_t(timestamp=0, channel=channel, subscriber_id="").encode(),
        )

    def spin_once(self, timeout_ms: int = 100) -> int:
        result = self._lcm.handle_timeout(timeout_ms)
        self._tick_reliable()
        return result

    def spin(self):
        while True:
            self._lcm.handle_timeout(100)
            self._tick_reliable()

    def close(self):
        for sub in self._subscriptions:
            self._lcm.unsubscribe(sub)
        self._subscriptions.clear()
        self._pending.clear()
