"""What the cross-language tests have in common: the message types they use, one channel of the
channel table for each, and a publisher and a subscriber in each language."""

import json
import time
from dataclasses import dataclass

import pytest

from raccoon_transport.types.raccoon import scalar_f_t, scalar_i32_t, string_t, vector3f_t
from integration_tests.conftest import Collector, reset_ring


@dataclass(frozen=True)
class Kind:
    name: str          # message type name, as in channels.yml
    cls: type          # the Python class
    channel: str       # a channel of the table that carries it (not reliable)
    values: dict       # example field values


KINDS = {
    "scalar_f_t": Kind("scalar_f_t", scalar_f_t, "hardware/odometry/state/vx", {"timestamp": 1000, "value": 3.140000104904175}),
    "scalar_i32_t": Kind("scalar_i32_t", scalar_i32_t, "hardware/watchdog/cmd/heartbeat", {"timestamp": 2000, "value": -7}),
    "vector3f_t": Kind("vector3f_t", vector3f_t, "hardware/chassis/cmd/velocity",
                       {"timestamp": 3000, "x": 1.0, "y": -2.5, "z": 0.25}),
    "string_t": Kind("string_t", string_t, "messages/warn", {"timestamp": 4000, "value": "grüße ✓"}),
}

TOOL = {"cpp": ("interop_publish", "interop_subscribe"), "dart": ("interop_publish.dart", "interop_subscribe.dart")}


def as_dict(message) -> dict:
    return {k: getattr(message, k) for k in message.__dataclass_fields__}


class Publisher:
    """Publishes `count` frames on a channel from Python, C++ or Dart."""

    def __init__(self, lang, transport, cpp_helper, dart_helper):
        self.lang, self.transport, self.cpp, self.dart = lang, transport, cpp_helper, dart_helper

    def publish(self, kind: Kind, count: int = 1, interval_ms: int = 0, extra=()):
        if self.lang == "py":
            for i in range(count):
                self.transport.publish(kind.channel, kind.cls(**kind.values))
                if interval_ms and i < count - 1:
                    time.sleep(interval_ms / 1000)
            return None
        launch = self.cpp if self.lang == "cpp" else self.dart
        tool = TOOL[self.lang][0]
        helper = launch(tool, [kind.channel, kind.name, json.dumps(kind.values, ensure_ascii=False), "--count", str(count),
                               "--interval-ms", str(interval_ms), *extra])
        helper.wait_for_event("published", timeout=10)
        return helper


class Subscriber:
    """Receives frames on a channel in Python, C++ or Dart, and is ready when it is created."""

    def __init__(self, lang, kind: Kind, transport, cpp_helper, dart_helper, count=1, timeout_ms=5000):
        self.lang, self.kind = lang, kind
        if lang == "py":
            self.collector = Collector(transport, kind.channel, kind.cls)
            return
        launch = cpp_helper if lang == "cpp" else dart_helper
        self.helper = launch(TOOL[lang][1], [kind.channel, kind.name, "--count", str(count),
                                              "--timeout-ms", str(timeout_ms)])
        self.helper.wait_for_event("subscribed", timeout=10)
        time.sleep(0.2)  # the reader is attached to the ring when the tool says "subscribed"

    def received(self, count=1, timeout=5.0) -> list[dict]:
        if self.lang == "py":
            assert self.collector.wait_for(count, timeout), f"python received {len(self.collector.frames)}/{count}"
            return [as_dict(m) for m in self.collector.frames[:count]]
        return [self.helper.wait_for_event("received", timeout=timeout)["data"] for _ in range(count)]


def prepare(kind: Kind) -> None:
    reset_ring(kind.channel)


def need(lang, request):
    """Ask for the fixture of a language; Python needs none."""
    return None if lang == "py" else request.getfixturevalue(f"{lang}_helper")


def approx_equal(got: dict, want: dict) -> None:
    for key, value in want.items():
        if isinstance(value, float):
            assert got[key] == pytest.approx(value, abs=1e-5), key
        else:
            assert got[key] == value, key
