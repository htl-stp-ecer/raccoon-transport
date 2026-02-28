"""Transport class wrapping LCM with optional reliable delivery."""

import logging
import lcm

logger = logging.getLogger(__name__)


class Transport:
    """Main transport class wrapping lcm.LCM with optional reliable delivery."""

    def __init__(self, provider: str = ""):
        self._lcm = lcm.LCM(provider) if provider else lcm.LCM()
        self._subscriptions = []

    @classmethod
    def create(cls, provider: str = "") -> "Transport":
        return cls(provider)

    def publish(self, channel: str, message, *, reliable: bool = False, retained: bool = False):
        """Publish an LCM message on the given channel."""
        if reliable or retained:
            logger.warning(
                "reliable/retained not yet implemented, "
                "falling back to plain publish on: %s",
                channel,
            )
        self._lcm.publish(channel, message.encode())

    def subscribe(self, channel: str, handler, *, reliable: bool = False, request_retained: bool = False):
        """Subscribe to messages on the given channel."""
        if reliable or request_retained:
            logger.warning(
                "reliable/retained not yet implemented, "
                "falling back to plain subscribe on: %s",
                channel,
            )
        sub = self._lcm.subscribe(channel, handler)
        self._subscriptions.append(sub)
        return sub

    def spin_once(self, timeout_ms: int = 100) -> int:
        """Handle a single pending message."""
        return self._lcm.handle_timeout(timeout_ms)

    def spin(self):
        """Block and handle messages indefinitely."""
        while True:
            self._lcm.handle()

    def close(self):
        """Unsubscribe all and clean up."""
        for sub in self._subscriptions:
            self._lcm.unsubscribe(sub)
        self._subscriptions.clear()
