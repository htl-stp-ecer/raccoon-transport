"""Stress and concurrency tests across all three languages."""

import json
import threading
import time

import pytest

from raccoon_transport import Transport
from raccoon_transport.types.raccoon import scalar_f_t, scalar_i32_t, vector3f_t
from integration_tests.conftest import spin_transport


# ---------------------------------------------------------------------------
# Test 1: Tri-language simultaneous publish
# All 3 languages publish 20 msgs on the same channel, one Python subscriber
# collects all 60.
# ---------------------------------------------------------------------------

def test_tri_lang_simultaneous_pub(transport, dart_helper, cpp_helper):
    channel = "test/stress/1/tri_pub"
    received = []
    stop = threading.Event()

    def handler(ch, data):
        msg = scalar_i32_t.decode(data)
        received.append(msg)

    transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received), daemon=True)
    t.start()
    time.sleep(0.2)

    # Launch Dart publisher: 20 messages
    dart_pub = dart_helper(
        "interop_publish.dart",
        [channel, "scalar_i32_t",
         json.dumps({"timestamp": 100, "value": 1}),
         "--count", "20", "--interval-ms", "10"],
    )

    # Launch C++ publisher: 20 messages
    cpp_pub = cpp_helper(
        "interop_publish",
        [channel, "scalar_i32_t",
         json.dumps({"timestamp": 200, "value": 2}),
         "--count", "20", "--interval-ms", "10"],
    )

    # Python publisher: 20 messages
    py_transport = Transport()
    try:
        for i in range(20):
            msg = scalar_i32_t()
            msg.timestamp = 300
            msg.value = 3
            py_transport.publish(channel, msg)
            time.sleep(0.01)
    finally:
        py_transport.close()

    # Wait for publishers to finish
    dart_pub.wait(timeout=10)
    cpp_pub.wait(timeout=10)

    # Give time for all messages to arrive
    time.sleep(1.0)
    stop.set()
    t.join(timeout=2)

    assert len(received) >= 60, f"Expected 60, got {len(received)}"


# ---------------------------------------------------------------------------
# Test 2: Tri-language simultaneous subscribe
# Python publishes 10 msgs, all 3 languages subscribe and verify receipt.
# ---------------------------------------------------------------------------

def test_tri_lang_simultaneous_sub(dart_helper, cpp_helper):
    channel = "test/stress/2/tri_sub"

    # Launch Dart subscriber
    dart_sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "scalar_f_t", "--count", "10", "--timeout-ms", "10000"],
    )
    dart_sub.wait_for_event("subscribed")

    # Launch C++ subscriber
    cpp_sub = cpp_helper(
        "interop_subscribe",
        [channel, "scalar_f_t", "--count", "10", "--timeout-ms", "10000"],
    )
    cpp_sub.wait_for_event("subscribed")

    # Python subscriber
    py_received = []
    py_transport = Transport()
    stop = threading.Event()

    def handler(ch, data):
        msg = scalar_f_t.decode(data)
        py_received.append(msg)

    py_transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(py_transport, stop, py_received), daemon=True)
    t.start()
    time.sleep(0.2)

    # Python publishes 10 messages
    pub_transport = Transport()
    try:
        for i in range(10):
            msg = scalar_f_t()
            msg.timestamp = 2000 + i
            msg.value = float(i)
            pub_transport.publish(channel, msg)
            time.sleep(0.02)
    finally:
        pub_transport.close()

    # Wait for all subscribers
    dart_done = dart_sub.wait_for_event("done", timeout=10)
    assert dart_done["count"] == 10

    cpp_done = cpp_sub.wait_for_event("done", timeout=10)
    assert cpp_done["count"] == 10

    time.sleep(0.5)
    stop.set()
    t.join(timeout=2)
    py_transport.close()

    assert len(py_received) >= 10


# ---------------------------------------------------------------------------
# Test 3: High-throughput Python -> Dart (100 messages rapid-fire)
# ---------------------------------------------------------------------------

def test_high_throughput_py_dart(dart_helper):
    channel = "test/stress/3/py_dart"
    count = 100

    sub = dart_helper(
        "interop_subscribe.dart",
        [channel, "scalar_i32_t", "--count", str(count), "--timeout-ms", "15000"],
    )
    sub.wait_for_event("subscribed")
    time.sleep(0.1)

    transport = Transport()
    try:
        for i in range(count):
            msg = scalar_i32_t()
            msg.timestamp = 3000 + i
            msg.value = i
            transport.publish(channel, msg)
    finally:
        transport.close()

    done = sub.wait_for_event("done", timeout=15)
    assert done["count"] == count


# ---------------------------------------------------------------------------
# Test 4: High-throughput C++ -> Python (100 messages rapid-fire)
# ---------------------------------------------------------------------------

def test_high_throughput_cpp_py(transport, cpp_helper):
    channel = "test/stress/4/cpp_py"
    count = 100
    received = []
    stop = threading.Event()

    def handler(ch, data):
        msg = scalar_i32_t.decode(data)
        received.append(msg)

    transport.subscribe(channel, handler)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received), daemon=True)
    t.start()
    time.sleep(0.1)

    pub = cpp_helper(
        "interop_publish",
        [channel, "scalar_i32_t",
         json.dumps({"timestamp": 4000, "value": 42}),
         "--count", str(count), "--interval-ms", "1"],
    )

    # Wait for all published events
    for _ in range(count):
        pub.wait_for_event("published", timeout=15)
    pub.wait(timeout=10)

    time.sleep(1.0)
    stop.set()
    t.join(timeout=2)

    assert len(received) >= count, f"Expected {count}, got {len(received)}"


# ---------------------------------------------------------------------------
# Test 5: Multi-channel concurrent
# 3 different channels simultaneously, each with a different language pair.
# ---------------------------------------------------------------------------

def test_multi_channel_concurrent(transport, dart_helper, cpp_helper):
    ch_a = "test/stress/5/chan_a"  # Py -> Dart
    ch_b = "test/stress/5/chan_b"  # C++ -> Py
    ch_c = "test/stress/5/chan_c"  # Dart -> C++

    # Channel A: Dart subscribes
    dart_sub = dart_helper(
        "interop_subscribe.dart",
        [ch_a, "scalar_f_t", "--count", "5", "--timeout-ms", "10000"],
    )
    dart_sub.wait_for_event("subscribed")

    # Channel B: Python subscribes
    received_b = []
    stop = threading.Event()

    def handler_b(ch, data):
        msg = vector3f_t.decode(data)
        received_b.append(msg)

    transport.subscribe(ch_b, handler_b)
    t = threading.Thread(target=spin_transport, args=(transport, stop, received_b), daemon=True)
    t.start()

    # Channel C: C++ subscribes
    cpp_sub = cpp_helper(
        "interop_subscribe",
        [ch_c, "scalar_i32_t", "--count", "5", "--timeout-ms", "10000"],
    )
    cpp_sub.wait_for_event("subscribed")
    time.sleep(0.2)

    # Channel A: Python publishes
    pub_a = Transport()
    for i in range(5):
        msg = scalar_f_t()
        msg.timestamp = 5100 + i
        msg.value = float(i)
        pub_a.publish(ch_a, msg)
        time.sleep(0.02)
    pub_a.close()

    # Channel B: C++ publishes
    cpp_pub = cpp_helper(
        "interop_publish",
        [ch_b, "vector3f_t",
         json.dumps({"timestamp": 5200, "x": 1.0, "y": 2.0, "z": 3.0}),
         "--count", "5", "--interval-ms", "20"],
    )

    # Channel C: Dart publishes
    dart_pub = dart_helper(
        "interop_publish.dart",
        [ch_c, "scalar_i32_t",
         json.dumps({"timestamp": 5300, "value": 99}),
         "--count", "5", "--interval-ms", "20"],
    )

    # Verify Channel A (Py -> Dart)
    dart_done = dart_sub.wait_for_event("done", timeout=10)
    assert dart_done["count"] == 5

    # Verify Channel B (C++ -> Py)
    cpp_pub.wait(timeout=10)
    time.sleep(0.5)
    stop.set()
    t.join(timeout=2)
    assert len(received_b) >= 5

    # Verify Channel C (Dart -> C++)
    cpp_done = cpp_sub.wait_for_event("done", timeout=10)
    assert cpp_done["count"] == 5
