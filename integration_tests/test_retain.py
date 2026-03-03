"""Retain protocol tests: within-language and cross-language."""

import json
import threading
import time

import pytest

from raccoon_transport import Transport
from raccoon_transport.types.raccoon import scalar_f_t
from integration_tests.conftest import spin_transport
from integration_tests.helpers.cpp_runner import CppHelper


# ---------------------------------------------------------------------------
# Test 7: Python retain -> Python request (two Transport instances)
# ---------------------------------------------------------------------------

def test_py_retain_py_request():
    publisher = Transport()
    subscriber = Transport()
    stop = threading.Event()
    received = []

    try:
        # Publish with retain
        msg = scalar_f_t()
        msg.timestamp = 7000
        msg.value = 7.77
        publisher.publish("test/retain/7", msg, retained=True)

        # Need to spin the publisher so it can respond to retain requests
        pub_thread = threading.Thread(
            target=spin_transport, args=(publisher, stop, []), daemon=True,
        )
        pub_thread.start()

        def handler(ch, data):
            m = scalar_f_t.decode(data)
            received.append(m)

        subscriber.subscribe("test/retain/7", handler, request_retained=True)

        # Spin subscriber to receive the retained value
        deadline = time.time() + 5
        while not received and time.time() < deadline:
            subscriber.spin_once(timeout_ms=50)

        assert len(received) >= 1
        assert received[0].timestamp == 7000
        assert received[0].value == pytest.approx(7.77, abs=1e-5)

    finally:
        stop.set()
        publisher.close()
        subscriber.close()


# ---------------------------------------------------------------------------
# Test 8: Dart retain -> Dart request (two Dart subprocesses)
# ---------------------------------------------------------------------------

def test_dart_retain_dart_request(dart_helper):
    channel = "test/retain/8"
    values = json.dumps({"timestamp": 8000, "value": 8.88})

    # Start publisher with --retained (stays alive)
    pub = dart_helper("interop_publish.dart", [
        channel, "scalar_f_t", values, "--retained",
    ])
    pub.wait_for_event("ready", timeout=10)

    # Start subscriber requesting retained value
    sub = dart_helper("interop_subscribe.dart", [
        channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
        "--request-retained",
    ])
    sub.wait_for_event("subscribed", timeout=5)

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 8000
    assert evt["data"]["value"] == pytest.approx(8.88, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 9: Python retain -> Dart request
# ---------------------------------------------------------------------------

def test_py_retain_dart_request(dart_helper):
    channel = "test/retain/9"
    publisher = Transport()
    stop = threading.Event()

    try:
        msg = scalar_f_t()
        msg.timestamp = 9000
        msg.value = 9.99
        publisher.publish(channel, msg, retained=True)

        # Spin publisher to respond to retain requests
        pub_thread = threading.Thread(
            target=spin_transport, args=(publisher, stop, []), daemon=True,
        )
        pub_thread.start()

        # Dart subscriber requests retained
        sub = dart_helper("interop_subscribe.dart", [
            channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
            "--request-retained",
        ])
        sub.wait_for_event("subscribed", timeout=5)

        evt = sub.wait_for_event("received", timeout=5)
        assert evt["data"]["timestamp"] == 9000
        assert evt["data"]["value"] == pytest.approx(9.99, abs=1e-5)

        sub.wait_for_event("done", timeout=5)

    finally:
        stop.set()
        publisher.close()


# ---------------------------------------------------------------------------
# Test 10: Dart retain -> Python request
# ---------------------------------------------------------------------------

def test_dart_retain_py_request(transport, dart_helper):
    channel = "test/retain/10"
    values = json.dumps({"timestamp": 10000, "value": 10.1})

    # Dart publisher with --retained
    pub = dart_helper("interop_publish.dart", [
        channel, "scalar_f_t", values, "--retained",
    ])
    pub.wait_for_event("ready", timeout=10)

    received = []

    def handler(ch, data):
        m = scalar_f_t.decode(data)
        received.append(m)

    transport.subscribe(channel, handler, request_retained=True)

    deadline = time.time() + 5
    while not received and time.time() < deadline:
        transport.spin_once(timeout_ms=50)

    assert len(received) >= 1
    assert received[0].timestamp == 10000
    assert received[0].value == pytest.approx(10.1, abs=1e-5)


# ---------------------------------------------------------------------------
# Test 11: Retain overwrites with latest value
# ---------------------------------------------------------------------------

def test_retain_overwrites_latest(dart_helper):
    channel = "test/retain/11"
    publisher = Transport()
    stop = threading.Event()

    try:
        # Publish first value
        msg1 = scalar_f_t()
        msg1.timestamp = 11001
        msg1.value = 1.0
        publisher.publish(channel, msg1, retained=True)

        # Overwrite with second value
        msg2 = scalar_f_t()
        msg2.timestamp = 11002
        msg2.value = 2.0
        publisher.publish(channel, msg2, retained=True)

        pub_thread = threading.Thread(
            target=spin_transport, args=(publisher, stop, []), daemon=True,
        )
        pub_thread.start()

        # Dart subscriber should get the latest (second) value
        sub = dart_helper("interop_subscribe.dart", [
            channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
            "--request-retained",
        ])
        sub.wait_for_event("subscribed", timeout=5)

        evt = sub.wait_for_event("received", timeout=5)
        assert evt["data"]["timestamp"] == 11002
        assert evt["data"]["value"] == pytest.approx(2.0, abs=1e-5)

        sub.wait_for_event("done", timeout=5)

    finally:
        stop.set()
        publisher.close()


# ---------------------------------------------------------------------------
# Test 12: Retain on wrong channel -> subscriber times out
# ---------------------------------------------------------------------------

def test_retain_wrong_channel_timeout(dart_helper):
    channel_cached = "test/retain/12/cached"
    channel_wrong = "test/retain/12/wrong"
    publisher = Transport()
    stop = threading.Event()

    try:
        msg = scalar_f_t()
        msg.timestamp = 12000
        msg.value = 12.0
        publisher.publish(channel_cached, msg, retained=True)

        pub_thread = threading.Thread(
            target=spin_transport, args=(publisher, stop, []), daemon=True,
        )
        pub_thread.start()

        # Request retained on a channel that has nothing cached
        sub = dart_helper("interop_subscribe.dart", [
            channel_wrong, "scalar_f_t", "--count", "1", "--timeout-ms", "1500",
            "--request-retained",
        ])
        sub.wait_for_event("subscribed", timeout=5)

        # Should time out (exit code 1)
        rc = sub.wait(timeout=5)
        assert rc == 1

    finally:
        stop.set()
        publisher.close()


# ---------------------------------------------------------------------------
# Test 13: C++ retain -> C++ request (two C++ subprocesses)
# ---------------------------------------------------------------------------

def test_cpp_retain_cpp_request(cpp_helper):
    channel = "test/retain/13"
    values = json.dumps({"timestamp": 13000, "value": 13.13})

    pub = cpp_helper("interop_publish", [
        channel, "scalar_f_t", values, "--retained",
    ])
    pub.wait_for_event("ready", timeout=10)

    sub = cpp_helper("interop_subscribe", [
        channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
        "--request-retained",
    ])
    sub.wait_for_event("subscribed", timeout=5)

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 13000
    assert evt["data"]["value"] == pytest.approx(13.13, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 14: C++ retain -> Python request
# ---------------------------------------------------------------------------

def test_cpp_retain_py_request(transport, cpp_helper):
    channel = "test/retain/14"
    values = json.dumps({"timestamp": 14000, "value": 14.14})

    pub = cpp_helper("interop_publish", [
        channel, "scalar_f_t", values, "--retained",
    ])
    pub.wait_for_event("ready", timeout=10)

    received = []

    def handler(ch, data):
        m = scalar_f_t.decode(data)
        received.append(m)

    transport.subscribe(channel, handler, request_retained=True)

    deadline = time.time() + 5
    while not received and time.time() < deadline:
        transport.spin_once(timeout_ms=50)

    assert len(received) >= 1
    assert received[0].timestamp == 14000
    assert received[0].value == pytest.approx(14.14, abs=1e-5)


# ---------------------------------------------------------------------------
# Test 15: Python retain -> C++ request
# ---------------------------------------------------------------------------

def test_py_retain_cpp_request(cpp_helper):
    channel = "test/retain/15"
    publisher = Transport()
    stop = threading.Event()

    try:
        msg = scalar_f_t()
        msg.timestamp = 15000
        msg.value = 15.15
        publisher.publish(channel, msg, retained=True)

        pub_thread = threading.Thread(
            target=spin_transport, args=(publisher, stop, []), daemon=True,
        )
        pub_thread.start()

        sub = cpp_helper("interop_subscribe", [
            channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
            "--request-retained",
        ])
        sub.wait_for_event("subscribed", timeout=5)

        evt = sub.wait_for_event("received", timeout=5)
        assert evt["data"]["timestamp"] == 15000
        assert evt["data"]["value"] == pytest.approx(15.15, abs=1e-5)

        sub.wait_for_event("done", timeout=5)

    finally:
        stop.set()
        publisher.close()


# ---------------------------------------------------------------------------
# Test 16: C++ retain -> Dart request
# ---------------------------------------------------------------------------

def test_cpp_retain_dart_request(dart_helper, cpp_helper):
    channel = "test/retain/16"
    values = json.dumps({"timestamp": 16000, "value": 16.16})

    pub = cpp_helper("interop_publish", [
        channel, "scalar_f_t", values, "--retained",
    ])
    pub.wait_for_event("ready", timeout=10)

    sub = dart_helper("interop_subscribe.dart", [
        channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
        "--request-retained",
    ])
    sub.wait_for_event("subscribed", timeout=5)

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 16000
    assert evt["data"]["value"] == pytest.approx(16.16, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 17: Dart retain -> C++ request
# ---------------------------------------------------------------------------

def test_dart_retain_cpp_request(dart_helper, cpp_helper):
    channel = "test/retain/17"
    values = json.dumps({"timestamp": 17000, "value": 17.17})

    pub = dart_helper("interop_publish.dart", [
        channel, "scalar_f_t", values, "--retained",
    ])
    pub.wait_for_event("ready", timeout=10)

    sub = cpp_helper("interop_subscribe", [
        channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000",
        "--request-retained",
    ])
    sub.wait_for_event("subscribed", timeout=5)

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 17000
    assert evt["data"]["value"] == pytest.approx(17.17, abs=1e-5)

    sub.wait_for_event("done", timeout=5)
