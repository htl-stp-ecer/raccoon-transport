"""Reliable (at-least-once) pub/sub integration tests — Python-to-Python."""

import threading
import time

import pytest

from raccoon_transport import Transport
from raccoon_transport.types.raccoon import scalar_f_t
from integration_tests.conftest import spin_transport


# ---------------------------------------------------------------------------
# Test: reliable publish -> reliable subscribe delivers message
# ---------------------------------------------------------------------------

def test_reliable_delivery():
    publisher = Transport()
    subscriber = Transport()
    stop = threading.Event()
    received = []

    try:
        def handler(ch, data):
            m = scalar_f_t.decode(data)
            received.append(m)

        subscriber.subscribe(
            "test/reliable/1", handler, reliable=True,
        )

        # Spin subscriber in background
        sub_thread = threading.Thread(
            target=spin_transport, args=(subscriber, stop, []), daemon=True,
        )
        sub_thread.start()

        # Give subscriber time to set up
        time.sleep(0.1)

        # Publish reliably
        msg = scalar_f_t()
        msg.timestamp = 1000
        msg.value = 42.0
        publisher.publish("test/reliable/1", msg, reliable=True)

        # Spin publisher so it can process ACKs
        deadline = time.time() + 5
        while not received and time.time() < deadline:
            publisher.spin_once(timeout_ms=50)

        assert len(received) >= 1
        assert received[0].timestamp == 1000
        assert received[0].value == pytest.approx(42.0, abs=1e-5)

    finally:
        stop.set()
        publisher.close()
        subscriber.close()


# ---------------------------------------------------------------------------
# Test: deduplication — retransmitted envelopes only delivered once
# ---------------------------------------------------------------------------

def test_reliable_dedup():
    publisher = Transport()
    subscriber = Transport()
    stop = threading.Event()
    received = []

    try:
        def handler(ch, data):
            m = scalar_f_t.decode(data)
            received.append(m)

        subscriber.subscribe(
            "test/reliable/dedup", handler, reliable=True,
        )

        sub_thread = threading.Thread(
            target=spin_transport, args=(subscriber, stop, []), daemon=True,
        )
        sub_thread.start()
        time.sleep(0.1)

        msg = scalar_f_t()
        msg.timestamp = 2000
        msg.value = 99.0
        publisher.publish("test/reliable/dedup", msg, reliable=True)

        # Spin long enough for retransmissions to fire
        deadline = time.time() + 2
        while time.time() < deadline:
            publisher.spin_once(timeout_ms=50)

        # Despite retransmissions, handler should only be called once
        assert len(received) == 1
        assert received[0].value == pytest.approx(99.0, abs=1e-5)

    finally:
        stop.set()
        publisher.close()
        subscriber.close()


# ---------------------------------------------------------------------------
# Test: max retries exhausted — message is dropped after maxRetries
# ---------------------------------------------------------------------------

def test_reliable_max_retries_exhausted():
    publisher = Transport()
    # No subscriber — ACKs will never arrive

    try:
        msg = scalar_f_t()
        msg.timestamp = 3000
        msg.value = 1.0
        publisher.publish(
            "test/reliable/timeout", msg,
            reliable=True, retry_interval_ms=50, max_retries=3,
        )

        assert len(publisher._pending) == 1

        # Spin until retries exhaust
        deadline = time.time() + 5
        while publisher._pending and time.time() < deadline:
            publisher.spin_once(timeout_ms=20)

        # Pending should be empty (dropped)
        assert len(publisher._pending) == 0

    finally:
        publisher.close()


# ---------------------------------------------------------------------------
# Test: multiple reliable messages on the same channel
# ---------------------------------------------------------------------------

def test_reliable_multiple_messages():
    publisher = Transport()
    subscriber = Transport()
    stop = threading.Event()
    received = []

    try:
        def handler(ch, data):
            m = scalar_f_t.decode(data)
            received.append(m)

        subscriber.subscribe(
            "test/reliable/multi", handler, reliable=True,
        )

        sub_thread = threading.Thread(
            target=spin_transport, args=(subscriber, stop, []), daemon=True,
        )
        sub_thread.start()
        time.sleep(0.1)

        for i in range(5):
            msg = scalar_f_t()
            msg.timestamp = 4000 + i
            msg.value = float(i)
            publisher.publish("test/reliable/multi", msg, reliable=True)

        deadline = time.time() + 5
        while len(received) < 5 and time.time() < deadline:
            publisher.spin_once(timeout_ms=50)

        assert len(received) == 5
        for i in range(5):
            assert received[i].timestamp == 4000 + i
            assert received[i].value == pytest.approx(float(i), abs=1e-5)

    finally:
        stop.set()
        publisher.close()
        subscriber.close()
