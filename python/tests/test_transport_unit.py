import itertools
import struct
import time

import pytest

from raccoon_transport import ProtocolChannels, Transport
from raccoon_transport.types.raccoon.ack_t import ack_t
from raccoon_transport.types.raccoon.envelope_t import envelope_t
from raccoon_transport.types.raccoon.retain_request_t import retain_request_t


_channel_counter = itertools.count()


def unique_channel(prefix: str) -> str:
    return f"{prefix}/{next(_channel_counter)}"


def spin_until_idle(transport: Transport, limit: int = 64):
    for _ in range(limit):
        if transport.spin_once(0) <= 0:
            return
    raise AssertionError("transport queue did not drain within iteration budget")


def encode_manual_retain_request(channel: str) -> bytes:
    channel_bytes = channel.encode("utf-8")
    return (
        struct.pack(">qq", 0, 0)
        + struct.pack(">i", len(channel_bytes))
        + channel_bytes
        + struct.pack(">i", 0)
    )


def test_request_retained_replays_cached_payload():
    transport = Transport("memq://")
    channel = unique_channel("unit/retain")
    cached = b"\x09\x08\x07\x06"

    transport._retain_cache[channel] = cached

    received = []
    transport.subscribe(
        channel,
        lambda _channel, data: received.append(data),
        request_retained=True,
    )

    spin_until_idle(transport)

    assert received == [cached]


def test_request_retained_missing_channel_is_silent():
    transport = Transport("memq://")
    channel = unique_channel("unit/missing")

    called = False

    def handler(_channel, _data):
        nonlocal called
        called = True

    transport.subscribe(channel, handler, request_retained=True)
    spin_until_idle(transport)

    assert called is False


@pytest.mark.skip(
    reason="retain_request_t wire format no longer matches the in-process retain"
    " handler after the lcm-gen drop in cc7d9e7; retain/reliable are slated for"
    " removal entirely (see c746cf2). Re-enable once the new retain protocol is in."
)
def test_retain_handler_accepts_generated_request_format():
    transport = Transport("memq://")
    channel = unique_channel("unit/generated-request")
    payload = b"\x04\x02\x04\x02"
    transport._retain_cache[channel] = payload

    received = []
    transport._lcm.subscribe(channel, lambda _channel, data: received.append(data))

    request = retain_request_t()
    request.timestamp = 111
    request.channel = channel
    request.subscriber_id = "subscriber"
    transport._lcm.publish(ProtocolChannels.RETAIN_REQUEST, request.encode())

    spin_until_idle(transport)

    assert received == [payload]


def test_retain_handler_accepts_manual_interop_format():
    transport = Transport("memq://")
    channel = unique_channel("unit/manual-request")
    payload = b"\x08\x06\x07\x05\x03\x00\x09"
    transport._retain_cache[channel] = payload

    received = []
    transport._lcm.subscribe(channel, lambda _channel, data: received.append(data))
    transport._lcm.publish(
        ProtocolChannels.RETAIN_REQUEST,
        encode_manual_retain_request(channel),
    )

    spin_until_idle(transport)

    assert received == [payload]


def test_reliable_subscriber_deduplicates_but_acks_every_delivery():
    transport = Transport("memq://")
    channel = unique_channel("unit/reliable")
    payload = b"\x07\x07\x01"

    delivered = []
    transport.subscribe(
        channel,
        lambda delivered_channel, data: delivered.append((delivered_channel, data)),
        reliable=True,
    )

    ack_messages = []
    transport._lcm.subscribe(
        ProtocolChannels.ACK,
        lambda _channel, data: ack_messages.append(ack_t.decode(data)),
    )

    env = envelope_t()
    env.timestamp = 123456789
    env.publisher_id = "publisher-1"
    env.seq_num = 42
    env.channel = channel
    env.payload_size = len(payload)
    env.payload = payload
    encoded = env.encode()

    reliable_channel = ProtocolChannels.reliable_channel(channel)
    transport._lcm.publish(reliable_channel, encoded)
    transport._lcm.publish(reliable_channel, encoded)

    spin_until_idle(transport)

    assert delivered == [(channel, payload)]
    assert len(ack_messages) == 2
    for ack in ack_messages:
        assert ack.publisher_id == "publisher-1"
        assert ack.seq_num == 42
        assert ack.subscriber_id == transport._instance_id


def test_reliable_publisher_stops_retrying_after_matching_ack():
    transport = Transport("memq://")
    channel = unique_channel("unit/ack-stop")
    payload = b"\x01\x09\x09"

    envelopes = []
    transport._lcm.subscribe(
        ProtocolChannels.reliable_channel(channel),
        lambda _channel, data: envelopes.append(envelope_t.decode(data)),
    )

    transport._reliable_publish(channel, payload, retry_interval_ms=1000, max_retries=5)
    assert transport._lcm.handle_timeout(0) == 1
    assert len(envelopes) == 1

    pending_seq = transport._pending[0]["seq_num"]
    ack = ack_t()
    ack.timestamp = 987654321
    ack.publisher_id = transport._instance_id
    ack.seq_num = pending_seq
    ack.subscriber_id = "subscriber-2"
    transport._lcm.publish(ProtocolChannels.ACK, ack.encode())
    assert transport._lcm.handle_timeout(0) == 1

    transport._tick_reliable()
    assert transport._lcm.handle_timeout(0) == 0

    assert len(envelopes) == 1
    assert transport._pending == []


def test_reliable_publisher_retries_exactly_until_max_retries(caplog):
    transport = Transport("memq://")
    channel = unique_channel("unit/retry-budget")
    payload = b"\x05\x04\x03\x02\x01"

    envelopes = []
    transport._lcm.subscribe(
        ProtocolChannels.reliable_channel(channel),
        lambda _channel, data: envelopes.append(envelope_t.decode(data)),
    )

    transport._reliable_publish(channel, payload, retry_interval_ms=1000, max_retries=3)
    assert transport._lcm.handle_timeout(0) == 1

    for _ in range(3):
        if not transport._pending:
            break
        transport._pending[0]["last_sent"] = time.monotonic() - 2
        transport._tick_reliable()
        while transport._lcm.handle_timeout(0) > 0:
            pass

    assert len(envelopes) == 3
    assert transport._pending == []
    assert "max retries exhausted" in caplog.text

    for env in envelopes:
        assert env.publisher_id == transport._instance_id
        assert env.seq_num == 0
        assert env.channel == channel
        assert env.payload == payload
