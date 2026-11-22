"""Publish in one language, receive in another: every pair of Python, C++ and Dart, for the message
types of the channel table, on the shared-memory rings."""

import pytest

from integration_tests.common import KINDS, Publisher, Subscriber, approx_equal, need, prepare

PAIRS = [("py", "cpp"), ("cpp", "py"), ("py", "dart"), ("dart", "py"), ("cpp", "dart"), ("dart", "cpp")]


@pytest.mark.parametrize("kind_name", list(KINDS))
@pytest.mark.parametrize("pub,sub", PAIRS)
def test_a_frame_arrives_unchanged(kind_name, pub, sub, request, transport):
    kind = KINDS[kind_name]
    cpp, dart = need("cpp", request) if "cpp" in (pub, sub) else None, need("dart", request) if "dart" in (pub, sub) else None
    prepare(kind)
    subscriber = Subscriber(sub, kind, transport, cpp, dart)
    Publisher(pub, transport, cpp, dart).publish(kind)
    [got] = subscriber.received()
    approx_equal(got, kind.values)


@pytest.mark.parametrize("pub,sub", [("py", "cpp"), ("cpp", "py")])
def test_ten_frames_arrive_in_order(pub, sub, request, transport):
    kind = KINDS["scalar_i32_t"]
    cpp = need("cpp", request)
    prepare(kind)
    subscriber = Subscriber(sub, kind, transport, cpp, None, count=10)
    if pub == "py":
        for i in range(10):
            transport.publish(kind.channel, kind.cls(timestamp=5000 + i, value=i))
    else:
        for i in range(10):
            cpp("interop_publish", [kind.channel, kind.name, '{"timestamp": %d, "value": %d}' % (5000 + i, i)]).wait_for_event("published")
    frames = subscriber.received(10)
    assert [f["value"] for f in frames] == list(range(10))
