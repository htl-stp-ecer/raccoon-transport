"""Wire-format round-trip tests without network."""

import json
import struct

import pytest

from raccoon_transport.types.raccoon import scalar_f_t, scalar_i32_t, string_t, vector3f_t
from integration_tests.helpers.dart_runner import DartHelper
from integration_tests.helpers.cpp_runner import CppHelper

TYPES_UNDER_TEST = ["scalar_f_t", "scalar_i32_t", "vector3f_t", "string_t"]

PY_CLASSES = {
    "scalar_f_t": scalar_f_t,
    "scalar_i32_t": scalar_i32_t,
    "vector3f_t": vector3f_t,
    "string_t": string_t,
}


def _py_fingerprint(type_name: str) -> int:
    cls = PY_CLASSES[type_name]
    return struct.unpack(">Q", cls._get_packed_fingerprint())[0]


# ---------------------------------------------------------------------------
# Test 13: Fingerprint match between Python and Dart for all shared types
# ---------------------------------------------------------------------------

def test_fingerprint_match(dart_helper):
    for type_name in TYPES_UNDER_TEST:
        py_fp = _py_fingerprint(type_name)

        sub = dart_helper("interop_subscribe.dart", ["--fingerprint", type_name])
        evt = sub.wait_for_event("fingerprint", timeout=10)
        dart_fp = int(evt["value"], 16)

        assert py_fp == dart_fp, (
            f"Fingerprint mismatch for {type_name}: "
            f"Python=0x{py_fp:016x}, Dart=0x{dart_fp:016x}"
        )


# ---------------------------------------------------------------------------
# Test 14: Python encode -> Dart decode
# ---------------------------------------------------------------------------

_TEST_MESSAGES = {
    "scalar_f_t": {"timestamp": 14001, "value": 1.5},
    "scalar_i32_t": {"timestamp": 14002, "value": -42},
    "vector3f_t": {"timestamp": 14003, "x": 0.1, "y": 0.2, "z": 0.3},
    "string_t": {"timestamp": 14004, "value": "cross-lang test"},
}


def _make_py_message(type_name: str, values: dict):
    cls = PY_CLASSES[type_name]
    msg = cls()
    for k, v in values.items():
        setattr(msg, k, v)
    return msg


def test_py_encode_dart_decode(dart_helper):
    for type_name, values in _TEST_MESSAGES.items():
        msg = _make_py_message(type_name, values)
        encoded = msg.encode()
        hex_str = encoded.hex()

        sub = dart_helper("interop_subscribe.dart", [
            "--decode-hex", type_name, hex_str,
        ])
        evt = sub.wait_for_event("decoded", timeout=10)
        data = evt["data"]

        if type_name == "scalar_f_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["value"] == pytest.approx(values["value"], abs=1e-5)
        elif type_name == "scalar_i32_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["value"] == values["value"]
        elif type_name == "vector3f_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["x"] == pytest.approx(values["x"], abs=1e-5)
            assert data["y"] == pytest.approx(values["y"], abs=1e-5)
            assert data["z"] == pytest.approx(values["z"], abs=1e-5)
        elif type_name == "string_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["value"] == values["value"]


# ---------------------------------------------------------------------------
# Test 15: Dart encode -> Python decode
# ---------------------------------------------------------------------------

def test_dart_encode_py_decode(dart_helper):
    for type_name, values in _TEST_MESSAGES.items():
        pub = dart_helper("interop_publish.dart", [
            "--encode-hex", type_name, json.dumps(values),
        ])
        evt = pub.wait_for_event("encoded", timeout=10)
        hex_str = evt["hex"]

        encoded = bytes.fromhex(hex_str)
        cls = PY_CLASSES[type_name]
        decoded = cls.decode(encoded)

        if type_name == "scalar_f_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.value == pytest.approx(values["value"], abs=1e-5)
        elif type_name == "scalar_i32_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.value == values["value"]
        elif type_name == "vector3f_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.x == pytest.approx(values["x"], abs=1e-5)
            assert decoded.y == pytest.approx(values["y"], abs=1e-5)
            assert decoded.z == pytest.approx(values["z"], abs=1e-5)
        elif type_name == "string_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.value == values["value"]


# ---------------------------------------------------------------------------
# Test 16: C++ fingerprint matches Python for all shared types
# ---------------------------------------------------------------------------

def test_cpp_fingerprint_match(cpp_helper):
    for type_name in TYPES_UNDER_TEST:
        py_fp = _py_fingerprint(type_name)

        sub = cpp_helper("interop_subscribe", ["--fingerprint", type_name])
        evt = sub.wait_for_event("fingerprint", timeout=10)
        cpp_fp = int(evt["value"], 16)

        assert py_fp == cpp_fp, (
            f"Fingerprint mismatch for {type_name}: "
            f"Python=0x{py_fp:016x}, C++=0x{cpp_fp:016x}"
        )


# ---------------------------------------------------------------------------
# Test 17: Python encode -> C++ decode
# ---------------------------------------------------------------------------

def test_py_encode_cpp_decode(cpp_helper):
    for type_name, values in _TEST_MESSAGES.items():
        msg = _make_py_message(type_name, values)
        encoded = msg.encode()
        hex_str = encoded.hex()

        sub = cpp_helper("interop_subscribe", [
            "--decode-hex", type_name, hex_str,
        ])
        evt = sub.wait_for_event("decoded", timeout=10)
        data = evt["data"]

        if type_name == "scalar_f_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["value"] == pytest.approx(values["value"], abs=1e-5)
        elif type_name == "scalar_i32_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["value"] == values["value"]
        elif type_name == "vector3f_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["x"] == pytest.approx(values["x"], abs=1e-5)
            assert data["y"] == pytest.approx(values["y"], abs=1e-5)
            assert data["z"] == pytest.approx(values["z"], abs=1e-5)
        elif type_name == "string_t":
            assert data["timestamp"] == values["timestamp"]
            assert data["value"] == values["value"]


# ---------------------------------------------------------------------------
# Test 18: C++ encode -> Python decode
# ---------------------------------------------------------------------------

def test_cpp_encode_py_decode(cpp_helper):
    for type_name, values in _TEST_MESSAGES.items():
        pub = cpp_helper("interop_publish", [
            "--encode-hex", type_name, json.dumps(values),
        ])
        evt = pub.wait_for_event("encoded", timeout=10)
        hex_str = evt["hex"]

        encoded = bytes.fromhex(hex_str)
        cls = PY_CLASSES[type_name]
        decoded = cls.decode(encoded)

        if type_name == "scalar_f_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.value == pytest.approx(values["value"], abs=1e-5)
        elif type_name == "scalar_i32_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.value == values["value"]
        elif type_name == "vector3f_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.x == pytest.approx(values["x"], abs=1e-5)
            assert decoded.y == pytest.approx(values["y"], abs=1e-5)
            assert decoded.z == pytest.approx(values["z"], abs=1e-5)
        elif type_name == "string_t":
            assert decoded.timestamp == values["timestamp"]
            assert decoded.value == values["value"]
