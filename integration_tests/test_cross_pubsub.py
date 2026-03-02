"""Cross-language pub/sub tests: Python <-> Dart <-> C++ via LCM multicast."""

import json
import threading
import time

import pytest

from raccoon_transport import Transport
from raccoon_transport.types.raccoon import scalar_f_t, scalar_i32_t, string_t, vector3f_t
from integration_tests.conftest import spin_transport


# ---------------------------------------------------------------------------
# Test 1: Python publishes scalar_f_t, Dart subscribes
# ---------------------------------------------------------------------------

def test_py_pub_dart_sub_scalar_f(dart_helper):
    channel = "test/cross/1/scalar_f"

    sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    transport = Transport()
    try:
        msg = scalar_f_t()
        msg.timestamp = 1000
        msg.value = 3.14
        transport.publish(channel, msg)
    finally:
        transport.close()

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 1000
    assert evt["data"]["value"] == pytest.approx(3.14, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 2: Dart publishes scalar_f_t, Python subscribes
# ---------------------------------------------------------------------------

def test_dart_pub_py_sub_scalar_f(transport, dart_helper):
    channel = "test/cross/2/scalar_f"
    received = []
    stop = threading.Event()

    def handler(ch, data):
        msg = scalar_f_t.decode(data)
        received.append(msg)

    transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received), daemon=True)
    t.start()
    time.sleep(0.1)

    pub = dart_helper(
        "interop_publish.dart",
        [channel, "scalar_f_t", json.dumps({"timestamp": 2000, "value": 2.718})],
    )
    pub.wait_for_event("published", timeout=5)
    pub.wait(timeout=5)

    # Give Python transport time to receive
    time.sleep(0.3)
    stop.set()
    t.join(timeout=2)

    assert len(received) >= 1
    assert received[0].timestamp == 2000
    assert received[0].value == pytest.approx(2.718, abs=1e-5)


# ---------------------------------------------------------------------------
# Test 3: Python publishes vector3f_t, Dart subscribes
# ---------------------------------------------------------------------------

def test_py_pub_dart_sub_vector3f(dart_helper):
    channel = "test/cross/3/vector3f"

    sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "vector3f_t", "--count", "1", "--timeout-ms", "5000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    transport = Transport()
    try:
        msg = vector3f_t()
        msg.timestamp = 3000
        msg.x = 1.0
        msg.y = 2.0
        msg.z = 3.0
        transport.publish(channel, msg)
    finally:
        transport.close()

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 3000
    assert evt["data"]["x"] == pytest.approx(1.0, abs=1e-5)
    assert evt["data"]["y"] == pytest.approx(2.0, abs=1e-5)
    assert evt["data"]["z"] == pytest.approx(3.0, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 4: Dart publishes string_t, Python subscribes
# ---------------------------------------------------------------------------

def test_dart_pub_py_sub_string(transport, dart_helper):
    channel = "test/cross/4/string"
    received = []
    stop = threading.Event()

    def handler(ch, data):
        msg = string_t.decode(data)
        received.append(msg)

    transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received), daemon=True)
    t.start()
    time.sleep(0.1)

    pub = dart_helper(
        "interop_publish.dart",
        [channel, "string_t", json.dumps({"timestamp": 4000, "value": "hello from dart"})],
    )
    pub.wait_for_event("published", timeout=5)
    pub.wait(timeout=5)

    time.sleep(0.3)
    stop.set()
    t.join(timeout=2)

    assert len(received) >= 1
    assert received[0].timestamp == 4000
    assert received[0].value == "hello from dart"


# ---------------------------------------------------------------------------
# Test 5: Dart publishes 10 scalar_i32_t, Python receives all
# ---------------------------------------------------------------------------

def test_dart_pub_10_py_sub_scalar_i32(transport, dart_helper):
    channel = "test/cross/5/scalar_i32"
    received = []
    stop = threading.Event()

    def handler(ch, data):
        msg = scalar_i32_t.decode(data)
        received.append(msg)

    transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received), daemon=True)
    t.start()
    time.sleep(0.1)

    pub = dart_helper(
        "interop_publish.dart",
        [
            channel, "scalar_i32_t",
            json.dumps({"timestamp": 5000, "value": 42}),
            "--count", "10",
            "--interval-ms", "20",
        ],
    )

    # Wait for all to be published
    for _ in range(10):
        pub.wait_for_event("published", timeout=10)
    pub.wait(timeout=5)

    time.sleep(0.5)
    stop.set()
    t.join(timeout=2)

    assert len(received) >= 10


# ---------------------------------------------------------------------------
# Test 6: Python publishes 10 scalar_i32_t, Dart receives all
# ---------------------------------------------------------------------------

def test_py_pub_10_dart_sub_scalar_i32(dart_helper):
    channel = "test/cross/6/scalar_i32"

    sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "scalar_i32_t", "--count", "10", "--timeout-ms", "10000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    transport = Transport()
    try:
        for i in range(10):
            msg = scalar_i32_t()
            msg.timestamp = 6000 + i
            msg.value = i * 10
            transport.publish(channel, msg)
            time.sleep(0.02)
    finally:
        transport.close()

    done = sub.wait_for_event("done", timeout=10)
    assert done["count"] == 10


# ---------------------------------------------------------------------------
# Test 7: Python publishes scalar_f_t, C++ subscribes
# ---------------------------------------------------------------------------

def test_py_pub_cpp_sub_scalar_f(cpp_helper):
    channel = "test/cross/7/scalar_f"

    sub = cpp_helper(
        "interop_subscribe",
        [channel, "scalar_f_t", "--count", "1", "--timeout-ms", "5000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    transport = Transport()
    try:
        msg = scalar_f_t()
        msg.timestamp = 7000
        msg.value = 3.14
        transport.publish(channel, msg)
    finally:
        transport.close()

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 7000
    assert evt["data"]["value"] == pytest.approx(3.14, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 8: C++ publishes scalar_f_t, Python subscribes
# ---------------------------------------------------------------------------

def test_cpp_pub_py_sub_scalar_f(transport, cpp_helper):
    channel = "test/cross/8/scalar_f"
    received = []
    stop = threading.Event()

    def handler(ch, data):
        msg = scalar_f_t.decode(data)
        received.append(msg)

    transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received), daemon=True)
    t.start()
    time.sleep(0.1)

    pub = cpp_helper(
        "interop_publish",
        [channel, "scalar_f_t", json.dumps({"timestamp": 8000, "value": 2.718})],
    )
    pub.wait_for_event("published", timeout=5)
    pub.wait(timeout=5)

    time.sleep(0.3)
    stop.set()
    t.join(timeout=2)

    assert len(received) >= 1
    assert received[0].timestamp == 8000
    assert received[0].value == pytest.approx(2.718, abs=1e-5)


# ---------------------------------------------------------------------------
# Test 9: C++ publishes vector3f_t, Dart subscribes
# ---------------------------------------------------------------------------

def test_cpp_pub_dart_sub_vector3f(dart_helper, cpp_helper):
    channel = "test/cross/9/vector3f"

    sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "vector3f_t", "--count", "1", "--timeout-ms", "5000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    pub = cpp_helper(
        "interop_publish",
        [channel, "vector3f_t", json.dumps({"timestamp": 9000, "x": 1.5, "y": 2.5, "z": 3.5})],
    )
    pub.wait_for_event("published", timeout=5)
    pub.wait(timeout=5)

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 9000
    assert evt["data"]["x"] == pytest.approx(1.5, abs=1e-5)
    assert evt["data"]["y"] == pytest.approx(2.5, abs=1e-5)
    assert evt["data"]["z"] == pytest.approx(3.5, abs=1e-5)

    sub.wait_for_event("done", timeout=5)


# ---------------------------------------------------------------------------
# Test 10: Dart publishes string_t, C++ subscribes
# ---------------------------------------------------------------------------

def test_dart_pub_cpp_sub_string(dart_helper, cpp_helper):
    channel = "test/cross/10/string"

    sub = cpp_helper(
        "interop_subscribe",
        [channel, "string_t", "--count", "1", "--timeout-ms", "5000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    pub = dart_helper(
        "interop_publish.dart",
        [channel, "string_t", json.dumps({"timestamp": 10000, "value": "hello from dart"})],
    )
    pub.wait_for_event("published", timeout=5)
    pub.wait(timeout=5)

    evt = sub.wait_for_event("received", timeout=5)
    assert evt["data"]["timestamp"] == 10000
    assert evt["data"]["value"] == "hello from dart"

    sub.wait_for_event("done", timeout=5)
