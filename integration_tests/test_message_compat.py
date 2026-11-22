"""The same message encodes to the same bytes in Python, C++ and Dart, and every language decodes
what another one encoded."""

import json

import pytest

from integration_tests.common import KINDS, approx_equal


def python_hex(kind) -> str:
    return kind.cls(**kind.values).encode().hex()


def tool_event(helper, name):
    return helper.wait_for_event(name, timeout=10)


@pytest.mark.parametrize("kind_name", list(KINDS))
@pytest.mark.parametrize("lang", ["cpp", "dart"])
def test_encoding_is_the_same_bytes(kind_name, lang, request):
    kind = KINDS[kind_name]
    launch = request.getfixturevalue(f"{lang}_helper")
    tool = "interop_publish" if lang == "cpp" else "interop_publish.dart"
    encoded = tool_event(launch(tool, ["--encode-hex", kind.name, json.dumps(kind.values, ensure_ascii=False)]), "encoded")
    assert encoded["hex"] == python_hex(kind)


@pytest.mark.parametrize("kind_name", list(KINDS))
@pytest.mark.parametrize("lang", ["cpp", "dart"])
def test_decoding_a_python_message(kind_name, lang, request):
    kind = KINDS[kind_name]
    launch = request.getfixturevalue(f"{lang}_helper")
    tool = "interop_subscribe" if lang == "cpp" else "interop_subscribe.dart"
    decoded = tool_event(launch(tool, ["--decode-hex", kind.name, python_hex(kind)]), "decoded")
    approx_equal(decoded["data"], kind.values)


@pytest.mark.parametrize("kind_name", list(KINDS))
@pytest.mark.parametrize("lang", ["cpp", "dart"])
def test_python_decodes_what_another_language_encoded(kind_name, lang, request):
    kind = KINDS[kind_name]
    launch = request.getfixturevalue(f"{lang}_helper")
    tool = "interop_publish" if lang == "cpp" else "interop_publish.dart"
    encoded = tool_event(launch(tool, ["--encode-hex", kind.name, json.dumps(kind.values, ensure_ascii=False)]), "encoded")
    message = kind.cls.decode(bytes.fromhex(encoded["hex"]))
    approx_equal({k: getattr(message, k) for k in kind.values}, kind.values)
