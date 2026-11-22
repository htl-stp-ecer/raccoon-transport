"""Load: many frames between languages, several channels at once."""

import time

import pytest

from integration_tests.common import KINDS, Publisher, Subscriber, need, prepare
from integration_tests.conftest import Collector, reset_ring
from raccoon_transport.types.raccoon import scalar_i32_t

HEARTBEAT = KINDS["scalar_i32_t"]


@pytest.mark.parametrize("pub,sub", [("py", "cpp"), ("cpp", "py")])
def test_two_hundred_frames_at_500_hz(pub, sub, request, transport):
    cpp = need("cpp", request)
    prepare(HEARTBEAT)
    subscriber = Subscriber(sub, HEARTBEAT, transport, cpp, None, count=190, timeout_ms=8000)
    if pub == "py":
        for i in range(200):
            transport.publish(HEARTBEAT.channel, scalar_i32_t(timestamp=10_000 + i, value=i))
            time.sleep(0.002)
    else:
        cpp("interop_publish", [HEARTBEAT.channel, HEARTBEAT.name, '{"timestamp": 10000, "value": 1}',
                                "--count", "200", "--interval-ms", "2"])
    if sub == "py":
        assert subscriber.collector.wait_for(190, 8), f"only {len(subscriber.collector.frames)} of 200 frames"
    else:
        assert subscriber.helper.wait_for_event("done", timeout=10)["count"] >= 190


def test_four_channels_at_once(request, transport):
    cpp = need("cpp", request)
    kinds = [KINDS[k] for k in ("scalar_f_t", "scalar_i32_t", "vector3f_t", "string_t")]
    for kind in kinds:
        prepare(kind)
    subscribers = [Subscriber("cpp", kind, transport, cpp, None, count=20, timeout_ms=8000) for kind in kinds]
    collectors = [Collector(transport, kind.channel, kind.cls) for kind in kinds]
    for _ in range(20):
        for kind in kinds:
            transport.publish(kind.channel, kind.cls(**kind.values))
        time.sleep(0.005)
    for subscriber in subscribers:
        assert subscriber.helper.wait_for_event("done", timeout=10)["count"] >= 20
    for collector in collectors:
        assert collector.wait_for(20, 5)


def test_many_python_subscribers_on_one_channel(transport):
    prepare(HEARTBEAT)
    collectors = [Collector(transport, HEARTBEAT.channel, HEARTBEAT.cls) for _ in range(20)]
    for i in range(50):
        transport.publish(HEARTBEAT.channel, scalar_i32_t(timestamp=20_000 + i, value=i))
    for collector in collectors:
        assert collector.wait_for(50, 5), len(collector.frames)
