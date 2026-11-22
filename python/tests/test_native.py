"""Tests of the native transport through its Python binding.

Run against an installed package that has the native module (pip install python/).
"""
import os
import threading
import time

import pytest

import raccoon_transport as rt
from raccoon_transport import channels
from raccoon_transport.types.raccoon import scalar_f_t, scalar_i32_t, string_t

pytestmark = pytest.mark.skipif(not rt.NATIVE, reason="no native module (pure install)")


def reset_ring(channel):
    path = "/dev/shm/raccoon-rings/" + channel.replace("/", ".")
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass


def wait_for(predicate, timeout=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.002)
    return predicate()


@pytest.fixture
def transport():
    t = rt.Transport()
    yield t
    t.shutdown()


def test_publish_and_subscribe_a_typed_message(transport):
    channel = channels.hardware.motor.cmd.power(0)
    reset_ring(channel)
    got = []
    transport.subscribe(channel, lambda ch, msg: got.append((ch, msg)), message_type=scalar_i32_t)
    transport.publish(channel, scalar_i32_t(timestamp=0, value=-1300))
    assert wait_for(lambda: len(got) == 1)
    ch, msg = got[0]
    assert ch == channel
    assert isinstance(msg, scalar_i32_t) and msg.value == -1300
    assert msg.timestamp > 0, "a timestamp of 0 is stamped"


def test_raw_subscription_gets_bytes(transport):
    channel = channels.hardware.motor.cmd.velocity(1)
    reset_ring(channel)
    got = []
    transport.subscribe(channel, lambda ch, data: got.append(data))
    payload = scalar_i32_t(timestamp=5, value=3).encode()
    transport.publish_raw(channel, payload)
    assert wait_for(lambda: got)
    assert got[0] == payload


def test_errors_say_why(transport):
    with pytest.raises(rt.PublishError) as info:
        transport.publish_raw("not/in/table", b"x" * 12)
    assert info.value.result == rt.PublishResult.NotInTable
    with pytest.raises(rt.PublishError) as info:
        transport.publish(channels.hardware.motor.cmd.power(0), scalar_f_t(timestamp=1, value=1.0))
    assert info.value.result == rt.PublishResult.TypeMismatch
    with pytest.raises(RuntimeError):
        transport.subscribe("not/in/table", lambda ch, d: None)
    with pytest.raises(RuntimeError):
        transport.subscribe(channels.hardware.motor.cmd.power(0), lambda ch, d: None, message_type=scalar_f_t)


def test_publish_returns_none_when_it_worked(transport):
    channel = channels.hardware.watchdog.cmd.HEARTBEAT
    assert transport.publish(channel, scalar_i32_t(timestamp=1, value=0)) is None
    assert transport.publish_raw(channel, scalar_i32_t(timestamp=2, value=0).encode()) is None


def test_a_raw_frame_of_the_wrong_size_is_a_type_mismatch(transport):
    with pytest.raises(rt.PublishError) as info:
        transport.publish_raw(channels.hardware.motor.cmd.power(0), b"123")
    assert info.value.result == rt.PublishResult.TypeMismatch


def test_a_frame_over_the_limit_of_the_channel_is_too_big(transport):
    # messages/error takes up to 1 MiB, protocol/ack up to 256 B (both in channels.yml).
    with pytest.raises(rt.PublishError) as info:
        transport.publish(channels.messages.ERROR, string_t(timestamp=1, value="x" * (1536 * 1024)))
    assert info.value.result == rt.PublishResult.TooBig
    with pytest.raises(rt.PublishError) as info:
        transport.publish_raw("protocol/ack", b"x" * 1000)
    assert info.value.result == rt.PublishResult.TooBig
    # A frame just under the limit still goes out.
    assert transport.publish(channels.messages.ERROR, string_t(timestamp=2, value="x" * 1000)) is None


def test_unsubscribe_stops_delivery(transport):
    channel = channels.hardware.motor.cmd.power(2)
    reset_ring(channel)
    a, b = [], []
    sub = transport.subscribe(channel, lambda ch, d: a.append(d))
    transport.subscribe(channel, lambda ch, d: b.append(d))
    transport.publish(channel, scalar_i32_t(timestamp=1, value=1))
    assert wait_for(lambda: a and b)
    transport.unsubscribe(sub)
    transport.publish(channel, scalar_i32_t(timestamp=2, value=2))
    assert wait_for(lambda: len(b) == 2)
    time.sleep(0.05)
    assert len(a) == 1


def test_retained_channel_replays_the_latest_value(transport):
    channel = channels.hardware.battery.state.VOLTAGE
    reset_ring(channel)
    transport.publish(channel, scalar_f_t(timestamp=1, value=7.5))
    transport.publish(channel, scalar_f_t(timestamp=2, value=7.25))
    got = []
    transport.subscribe(channel, lambda ch, m: got.append(m.value), message_type=scalar_f_t)
    assert wait_for(lambda: got)
    time.sleep(0.05)
    assert got == [7.25]


def test_handler_errors_are_reported_not_fatal(transport):
    channel = channels.hardware.motor.cmd.power(3)
    reset_ring(channel)
    errors = []
    transport.set_handler_error_handler(lambda ch, what: errors.append((ch, what)))
    later = []

    def boom(ch, data):
        raise ValueError("boom")

    transport.subscribe(channel, boom)
    transport.subscribe(channel, lambda ch, d: later.append(d))
    transport.publish(channel, scalar_i32_t(timestamp=1, value=1))
    assert wait_for(lambda: later)
    assert wait_for(lambda: errors)


def test_stats_and_shutdown(transport):
    channel = channels.hardware.watchdog.cmd.HEARTBEAT
    reset_ring(channel)
    got = []
    transport.subscribe(channel, lambda ch, d: got.append(d))
    for i in range(5):
        transport.publish(channel, scalar_i32_t(timestamp=i + 1, value=i))
    assert wait_for(lambda: len(got) == 5)
    stats = transport.get_and_reset_stats()
    assert stats["channels"][0]["name"] == channel
    assert stats["channels"][0]["deliveries"] == 5
    transport.shutdown()
    assert not transport.is_alive()
    with pytest.raises(rt.PublishError) as info:
        transport.publish_raw(channel, scalar_i32_t(timestamp=1, value=0).encode())
    assert info.value.result == rt.PublishResult.Closed


def test_get_transport_is_shared_and_restartable():
    first = rt.get_transport()
    assert rt.get_transport() is first
    first.shutdown()
    assert rt.get_transport() is not first
    # leave a transport that the exit hook can shut down
