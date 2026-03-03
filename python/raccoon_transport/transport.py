"""Transport class wrapping LCM with optional reliable delivery."""

import logging
import struct
import lcm

from .channels import ProtocolChannels

logger = logging.getLogger(__name__)


class Transport:
    """Main transport class wrapping lcm.LCM with optional reliable delivery."""

    def __init__(self, provider: str = ""):
        """Create a transport bound to the given LCM provider URL."""
        self._lcm = lcm.LCM(provider) if provider else lcm.LCM()
        self._subscriptions = []
        self._retain_cache: dict[str, bytes] = {}

        # Subscribe to retain requests so we can replay cached values
        self._lcm.subscribe(ProtocolChannels.RETAIN_REQUEST, self._on_retain_request)

    @classmethod
    def create(cls, provider: str = "") -> "Transport":
        """Construct a transport instance.

        This is a convenience alternate constructor for callers that prefer a
        factory-style API.
        """
        return cls(provider)

    def publish(self, channel: str, message, *, reliable: bool = False, retained: bool = False):
        """Publish an encoded LCM message on ``channel``.

        Args:
            channel: Destination channel name.
            message: LCM message instance with an ``encode()`` method.
            reliable: Reserved for future reliable-delivery support. Currently
                logs a warning and falls back to normal publish.
            retained: When ``True``, cache the encoded payload so it can be
                replayed to future subscribers that request retained state.
        """
        if reliable:
            logger.warning(
                "reliable not yet implemented, "
                "falling back to plain publish on: %s",
                channel,
            )
        encoded = message.encode()
        self._lcm.publish(channel, encoded)
        if retained:
            self._retain_cache[channel] = encoded

    def subscribe(self, channel: str, handler, *, reliable: bool = False, request_retained: bool = False):
        """Subscribe to messages on ``channel``.

        Args:
            channel: Channel to subscribe to.
            handler: Callback passed directly to ``lcm.LCM.subscribe``.
            reliable: Reserved for future reliable-delivery support. Currently
                logs a warning and falls back to normal subscribe.
            request_retained: When ``True``, publish a retain request after the
                subscription is created so any cached state is replayed.

        Returns:
            The subscription handle returned by ``lcm.LCM.subscribe``.
        """
        if reliable:
            logger.warning(
                "reliable not yet implemented, "
                "falling back to plain subscribe on: %s",
                channel,
            )
        sub = self._lcm.subscribe(channel, handler)
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
        """Send a retain request for ``channel`` on the internal protocol bus."""
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
        """Handle at most one pending message and return the LCM status code."""
        return self._lcm.handle_timeout(timeout_ms)

    def spin(self):
        """Block and handle messages indefinitely."""
        while True:
            self._lcm.handle()

    def close(self):
        """Unsubscribe all active handlers owned by this transport instance."""
        for sub in self._subscriptions:
            self._lcm.unsubscribe(sub)
        self._subscriptions.clear()
