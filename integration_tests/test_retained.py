"""Retained channels: a new subscriber gets the latest frame once. Whether a channel is retained is
a property of the channel table; hardware/odometry/state/vx is, hardware/watchdog/cmd/heartbeat is not."""

import time

import pytest

from integration_tests.common import KINDS, Publisher, Subscriber, need, prepare

RETAINED = KINDS["scalar_f_t"]
PLAIN = KINDS["scalar_i32_t"]


@pytest.mark.parametrize("pub,sub", [("py", "cpp"), ("cpp", "py"), ("py", "dart"), ("dart", "py")])
def test_a_late_subscriber_gets_the_latest_frame(pub, sub, request, transport):
    cpp, dart = need("cpp", request) if "cpp" in (pub, sub) else None, need("dart", request) if "dart" in (pub, sub) else None
    prepare(RETAINED)
    publisher = Publisher(pub, transport, cpp, dart)
    publisher.publish(RETAINED, count=3)  # three frames with the same values: the subscriber gets one
    time.sleep(0.2)
    subscriber = Subscriber(sub, RETAINED, transport, cpp, dart, count=2, timeout_ms=1500)
    [got] = subscriber.received(1)
    assert got["value"] == pytest.approx(RETAINED.values["value"], abs=1e-5)
    if sub == "py":
        time.sleep(0.3)
        assert len(subscriber.collector.frames) == 1, "the latest frame must come once"


def test_a_channel_that_is_not_retained_replays_nothing(request, transport):
    cpp = need("cpp", request)
    prepare(PLAIN)
    Publisher("py", transport, cpp, None).publish(PLAIN, count=3)
    time.sleep(0.2)
    helper = cpp("interop_subscribe", [PLAIN.channel, PLAIN.name, "--count", "1", "--timeout-ms", "1000"])
    helper.wait_for_event("subscribed")
    event = helper.wait_for_event("timeout", timeout=5)
    assert event["count"] == 0


def test_every_subscriber_of_a_retained_channel_gets_it(request, transport):
    cpp = need("cpp", request)
    prepare(RETAINED)
    Publisher("py", transport, cpp, None).publish(RETAINED)
    time.sleep(0.2)
    first = Subscriber("cpp", RETAINED, transport, cpp, None)
    second = Subscriber("cpp", RETAINED, transport, cpp, None)
    assert first.received()[0]["timestamp"] == RETAINED.values["timestamp"]
    assert second.received()[0]["timestamp"] == RETAINED.values["timestamp"]
