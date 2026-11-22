"""Reliable channels (reliable in the channel table): the receiver acknowledges every frame and gets
each command once, the sender repeats a frame until it is acknowledged and reports a give-up.
hardware/motor/0/cmd/power is a reliable channel."""

import json
import time

import pytest

from integration_tests.conftest import Collector, reset_ring
from raccoon_transport.types.raccoon import scalar_i32_t

CHANNEL = "hardware/motor/0/cmd/power"


def frame(timestamp: int, value: int) -> scalar_i32_t:
    return scalar_i32_t(timestamp=timestamp, value=value)


@pytest.fixture()
def events(transport):
    seen = []
    transport.set_reliable_event_handler(lambda kind, channel, key, attempts: seen.append((kind, channel, key)))
    return seen


def test_a_command_reaches_a_subscriber_in_another_process(request, transport, events):
    cpp = request.getfixturevalue("cpp_helper")
    reset_ring(CHANNEL)
    reset_ring("protocol/ack")
    helper = cpp("interop_subscribe", [CHANNEL, "scalar_i32_t", "--count", "1", "--timeout-ms", "5000", "--reliable"])
    helper.wait_for_event("subscribed")
    time.sleep(0.2)
    transport.publish(CHANNEL, frame(7000, 50))
    assert helper.wait_for_event("received", timeout=5)["data"]["value"] == 50
    time.sleep(0.4)  # the acknowledgement is on its way back
    stats = transport.get_and_reset_stats()
    assert stats["reliable_dropped"] == 0
    assert not [e for e in events if e[0] == "gave_up"]


def test_without_a_subscriber_the_sender_gives_up_loudly(transport, events):
    reset_ring(CHANNEL)
    transport.publish(CHANNEL, frame(8000, 1))
    deadline = time.monotonic() + 4
    while time.monotonic() < deadline and not [e for e in events if e[0] == "gave_up"]:
        time.sleep(0.05)
    assert [e for e in events if e[0] == "gave_up"], "no give-up was reported"
    assert any(e[0] == "retransmit" for e in events), "the sender did not repeat the frame first"
    assert transport.get_and_reset_stats()["reliable_dropped"] >= 1


def test_a_repeated_frame_is_delivered_once(request, transport):
    cpp = request.getfixturevalue("cpp_helper")
    reset_ring(CHANNEL)
    helper = cpp("interop_subscribe", [CHANNEL, "scalar_i32_t", "--count", "3", "--timeout-ms", "2000", "--reliable"])
    helper.wait_for_event("subscribed")
    time.sleep(0.2)
    same = frame(9000, 1).encode()
    for payload in (same, same, same, frame(9001, 2).encode()):
        transport.publish_raw(CHANNEL, payload)
    # A, A, A, B is two commands: asking for three runs into the timeout with a count of two.
    assert helper.wait_for_event("timeout", timeout=6)["count"] == 2


def test_the_same_value_with_another_time_stamp_is_a_new_command(request, transport):
    cpp = request.getfixturevalue("cpp_helper")
    reset_ring(CHANNEL)
    helper = cpp("interop_subscribe", [CHANNEL, "scalar_i32_t", "--count", "3", "--timeout-ms", "3000", "--reliable"])
    helper.wait_for_event("subscribed")
    time.sleep(0.2)
    for ts in (9100, 9101, 9102):
        transport.publish(CHANNEL, frame(ts, 5))
    assert helper.wait_for_event("done", timeout=5)["count"] == 3


def test_a_command_from_cpp_reaches_python_once(request, transport):
    cpp = request.getfixturevalue("cpp_helper")
    reset_ring(CHANNEL)
    reset_ring("protocol/ack")
    collector = Collector(transport, CHANNEL, scalar_i32_t)
    time.sleep(0.2)
    cpp("interop_publish", [CHANNEL, "scalar_i32_t", json.dumps({"timestamp": 9200, "value": 9}), "--reliable"])
    assert collector.wait_for(1, 5)
    time.sleep(0.8)  # the sender would repeat the frame for a second if it were not acknowledged
    assert len(collector.frames) == 1
