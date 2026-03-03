"""Reliable (at-least-once) pub/sub integration tests."""

import json
import threading
import time

import pytest

from raccoon_transport import Transport
from raccoon_transport.types.raccoon import scalar_f_t, scalar_i32_t
from integration_tests.conftest import spin_transport


# ---------------------------------------------------------------------------
# Python-to-Python tests
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


# ---------------------------------------------------------------------------
# Cross-language: Dart publish -> C++ subscribe (reliable)
# ---------------------------------------------------------------------------

def test_dart_pub_cpp_sub_reliable(dart_helper, cpp_helper):
    """Dart publishes reliably, C++ subscribes reliably — the exact path
    used by the Flutter UI to send shutdown commands to stm32-data-reader."""
    channel = "test/reliable/dart_cpp"

    # Start C++ subscriber first (reliable)
    sub = cpp_helper(
        "interop_subscribe",
        [channel, "scalar_i32_t", "--count", "1", "--timeout-ms", "5000",
         "--reliable"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    # Dart publishes reliably
    pub = dart_helper(
        "interop_publish.dart",
        [channel, "scalar_i32_t",
         json.dumps({"timestamp": 9000, "value": 42}),
         "--reliable"],
    )
    pub.wait_for_event("published", timeout=5)

    # Wait for C++ to receive
    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 9000
    assert evt["data"]["value"] == 42

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Cross-language: C++ publish -> Dart subscribe (reliable)
# ---------------------------------------------------------------------------

def test_cpp_pub_dart_sub_reliable(dart_helper, cpp_helper):
    """C++ publishes reliably, Dart subscribes reliably."""
    channel = "test/reliable/cpp_dart"

    # Start Dart subscriber first (reliable)
    sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "scalar_i32_t", "--count", "1", "--timeout-ms", "5000",
         "--reliable"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    # C++ publishes reliably
    pub = cpp_helper(
        "interop_publish",
        [channel, "scalar_i32_t",
         json.dumps({"timestamp": 9001, "value": 77}),
         "--reliable"],
    )
    pub.wait_for_event("published", timeout=5)

    # Wait for Dart to receive
    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 9001
    assert evt["data"]["value"] == 77

    sub.wait_for_event("done", timeout=5)
