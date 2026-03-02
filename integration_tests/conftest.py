"""Fixtures for cross-language integration tests."""

import threading

import pytest

from raccoon_transport import Transport
from integration_tests.helpers.dart_runner import DartHelper
from integration_tests.helpers.cpp_runner import CppHelper


@pytest.fixture()
def transport():
    """Create a Python Transport instance, closed after the test."""
    t = Transport()
    yield t
    t.close()


@pytest.fixture()
def dart_helper():
    """Factory fixture: tracks all launched DartHelpers and kills them on teardown."""
    helpers: list[DartHelper] = []

    def _launch(script: str, args: list[str]) -> DartHelper:
        h = DartHelper(script, args)
        helpers.append(h)
        return h

    yield _launch

    for h in helpers:
        h.stop()


@pytest.fixture()
def cpp_helper():
    """Factory fixture: tracks all launched CppHelpers and kills them on teardown."""
    helpers: list[CppHelper] = []

    def _launch(script: str, args: list[str]) -> CppHelper:
        h = CppHelper(script, args)
        helpers.append(h)
        return h

    yield _launch

    for h in helpers:
        h.stop()


def spin_transport(transport: Transport, stop_event: threading.Event, received: list):
    """Run spin_once in a background thread, appending (channel, data) to received."""
    while not stop_event.is_set():
        transport.spin_once(timeout_ms=50)
