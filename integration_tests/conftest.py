"""Fixtures for the cross-language integration tests.

The tests run the C++ transport (through the interop tools of cpp/tools), the Dart transport
(through dart/bin and the C bridge) and Python in separate processes that talk through the
shared-memory rings. C++ tools are in <build dir>/cpp/tools (RACCOON_BUILD_DIR), the Dart tools
need `dart` and cpp/bridge/libraccoon_transport_bridge.so (RACCOON_BRIDGE_DIR); a test that needs
a tool that is not there is skipped.
"""

import os
import threading

import pytest

import raccoon_transport as rt
from integration_tests.helpers import cpp_runner, dart_runner
from integration_tests.helpers.cpp_runner import CppHelper
from integration_tests.helpers.dart_runner import DartHelper

RING_DIR = "/dev/shm/raccoon-rings"


def pytest_configure(config):
    config.addinivalue_line("markers", "dart: the test starts a Dart tool")


def pytest_collection_modifyitems(items):
    # A test that involves Dart has "dart" in its id (the parametrised pairs) or in its name.
    for item in items:
        if "dart" in item.nodeid.lower():
            item.add_marker(pytest.mark.dart)


def reset_ring(channel: str) -> None:
    """Delete the ring file of a channel so that frames of an earlier test are not seen."""
    try:
        os.unlink(os.path.join(RING_DIR, channel.replace("/", ".")))
    except FileNotFoundError:
        pass


@pytest.fixture(autouse=True)
def need_native_transport():
    if not rt.NATIVE:
        pytest.skip("raccoon_transport has no native module")


@pytest.fixture()
def transport():
    """A Python Transport, shut down after the test."""
    t = rt.Transport()
    yield t
    t.shutdown()


@pytest.fixture()
def cpp_helper():
    """Factory fixture: starts a C++ interop tool and stops all of them at the end."""
    if not os.path.isdir(cpp_runner.BUILD_DIR):
        pytest.skip(f"no C++ interop tools in {cpp_runner.BUILD_DIR}")
    helpers: list[CppHelper] = []

    def _launch(script: str, args: list[str]) -> CppHelper:
        h = CppHelper(script, args)
        helpers.append(h)
        return h

    yield _launch
    for h in helpers:
        h.stop()


@pytest.fixture()
def dart_helper():
    """Factory fixture: starts a Dart tool and stops all of them at the end."""
    if not dart_runner.available():
        pytest.skip("no dart, or no libraccoon_transport_bridge.so")
    helpers: list[DartHelper] = []

    def _launch(script: str, args: list[str]) -> DartHelper:
        h = DartHelper(script, args)
        helpers.append(h)
        return h

    yield _launch
    for h in helpers:
        h.stop()


class Collector:
    """Frames a Python subscription received, decoded into dictionaries."""

    def __init__(self, transport, channel: str, message_type):
        self.frames: list = []
        self._lock = threading.Lock()
        transport.subscribe(channel, self._on_frame, message_type=message_type)

    def _on_frame(self, _channel, message) -> None:
        with self._lock:
            self.frames.append(message)

    def wait_for(self, count: int, timeout: float = 5.0) -> bool:
        import time

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if len(self.frames) >= count:
                    return True
            time.sleep(0.005)
        with self._lock:
            return len(self.frames) >= count
