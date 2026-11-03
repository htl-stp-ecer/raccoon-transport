#!/usr/bin/env python3
"""Compute the raccoon-transport contract hash and keep every language in sync.

The contract is everything that components rely on when they exchange data:

* the wire layout of every message type (name, ordered fields, field types),
* every channel name (parametric channels as a pattern),
* the raccoon_ring layout (magic and version).

The single source is the C++ side (``messages/include`` and ``cpp/include``),
because that is the code that defines the ring layout and the encoder order.
This script reads it, writes ``contract/contract.json`` (the canonical,
human-readable form) and the generated constants for C++, Python and Dart.

The hash is the first 16 hex characters of the SHA-256 of the canonical JSON.
Comments, whitespace, default values and declaration order of channels do not
change it. The order of fields inside a message does, because it is the wire
order. The transport version number is deliberately not part of the hash.

Usage:
    scripts/contract.py --write     regenerate contract.json and the constants
    scripts/contract.py --check     fail if anything generated is out of date,
                                    or if Python/Dart messages drift from C++
    scripts/contract.py --print     print the hash
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HASH_LENGTH = 16
MARKER_PREFIX = "RACCOON_CONTRACT_V1="
FORMAT = 1

MESSAGES_DIR = ROOT / "messages" / "include" / "raccoon"
CHANNELS_H = ROOT / "cpp" / "include" / "raccoon" / "Channels.h"
RING_H = ROOT / "cpp" / "include" / "raccoon" / "raccoon_ring.h"
PY_TYPES_DIR = ROOT / "python" / "raccoon_transport" / "types" / "raccoon"
DART_TYPES_DIR = ROOT / "dart" / "lib" / "messages"

CONTRACT_JSON = ROOT / "contract" / "contract.json"
CPP_OUT = ROOT / "cpp" / "include" / "raccoon" / "Contract.h"
PY_OUT = ROOT / "python" / "raccoon_transport" / "_contract.py"
DART_OUT = ROOT / "dart" / "lib" / "src" / "contract.dart"

SCALARS = {
    "int8_t": "i8",
    "int16_t": "i16",
    "int32_t": "i32",
    "int64_t": "i64",
    "uint8_t": "u8",
    "uint16_t": "u16",
    "uint32_t": "u32",
    "uint64_t": "u64",
    "float": "f32",
    "double": "f64",
    "bool": "bool",
    "std::string": "string",
}


class ContractError(Exception):
    pass


# ── Reading the C++ source ────────────────────────────────────────────────


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _field_type(raw: str, known: set[str], where: str) -> str:
    raw = raw.strip()
    if raw in SCALARS:
        return SCALARS[raw]
    arr = re.fullmatch(r"std::array<\s*([\w:]+)\s*,\s*(\d+)\s*>", raw)
    if arr:
        return f"array<{_field_type(arr.group(1), known, where)},{arr.group(2)}>"
    vec = re.fullmatch(r"std::vector<\s*([\w:]+)\s*>", raw)
    if vec:
        inner = vec.group(1)
        if inner == "uint8_t":
            return "bytes"
        return f"list<{_field_type(inner, known, where)}>"
    if re.fullmatch(r"\w+_t", raw):
        return f"struct:{raw}"
    raise ContractError(f"{where}: unsupported field type {raw!r}")


def read_types() -> dict[str, list[list[str]]]:
    types: dict[str, list[list[str]]] = {}
    for header in sorted(MESSAGES_DIR.glob("*_t.hpp")):
        text = _strip_comments(header.read_text())
        match = re.search(r"struct\s+(\w+)\s*\{(.*?)\n\s*\};", text, flags=re.S)
        if not match:
            raise ContractError(f"{header.name}: no struct found")
        name, body = match.groups()
        fields: list[list[str]] = []
        for line in body.split(";"):
            line = line.strip()
            if not line or "(" in line:
                continue  # method declaration
            line = re.sub(r"\s*(=.*|\{.*\})$", "", line, flags=re.S)  # default value
            decl = re.fullmatch(r"(.+?)\s+(\w+)", line, flags=re.S)
            if not decl:
                raise ContractError(f"{header.name}: cannot parse {line!r}")
            ftype, fname = decl.groups()
            fields.append([fname, _field_type(ftype, set(), f"{name}.{fname}")])
        if not fields:
            raise ContractError(f"{header.name}: struct {name} has no fields")
        types[name] = fields
    known = set(types)
    for name, fields in types.items():
        for fname, ftype in fields:
            for ref in re.findall(r"struct:(\w+)", ftype):
                if ref not in known:
                    raise ContractError(f"{name}.{fname}: unknown type {ref}")
    return dict(sorted(types.items()))


def read_channels() -> list[str]:
    text = _strip_comments(CHANNELS_H.read_text())
    channels: set[str] = set()
    for value in re.findall(r'constexpr\s+auto\s+\w+\s*=\s*"([^"]+)"', text):
        channels.add(value)
    # Parametric channels: RACCOON_CACHED_CHANNEL("prefix", "suffix") -> prefix{port}suffix
    for prefix, suffix in re.findall(
        r'RACCOON_CACHED_CHANNEL\(\s*"([^"]*)"\s*,\s*"([^"]*)"\s*\)', text
    ):
        channels.add(f"{prefix}{{port}}{suffix}")
    # Reliable wrapper channels are built from this prefix.
    for prefix in re.findall(r'return\s+"(__raccoon/[^"]+)"\s*\+', text):
        channels.add(f"{prefix}{{channel}}")
    if not channels:
        raise ContractError("no channels found in Channels.h")
    return sorted(channels)


def read_ring() -> dict[str, int]:
    text = _strip_comments(RING_H.read_text())
    values = {}
    for key in ("RRB_MAGIC", "RRB_VERSION"):
        match = re.search(rf"#define\s+{key}\s+(0x[0-9a-fA-F]+|\d+)u?", text)
        if not match:
            raise ContractError(f"{key} not found in raccoon_ring.h")
        values[key] = int(match.group(1), 0)
    return {"magic": values["RRB_MAGIC"], "version": values["RRB_VERSION"]}


def build_contract() -> dict:
    return {
        "format": FORMAT,
        "ring": read_ring(),
        "channels": read_channels(),
        "types": read_types(),
    }


# ── Hash ─────────────────────────────────────────────────────────────────


def canonical(contract: dict) -> str:
    return json.dumps(contract, sort_keys=True, separators=(",", ":"))


def contract_hash(contract: dict) -> str:
    return hashlib.sha256(canonical(contract).encode()).hexdigest()[:HASH_LENGTH]


# ── Generated files ──────────────────────────────────────────────────────

HEADER = "Generated by scripts/contract.py - do not edit by hand."


def render_contract_json(contract: dict, digest: str) -> str:
    document = {"hash": digest, **contract}
    return json.dumps(document, indent=2, sort_keys=False) + "\n"


def render_cpp(digest: str) -> str:
    return f"""#pragma once
// {HEADER}

// Same value as a string literal, for compile-time concatenation of the marker.
#define RACCOON_CONTRACT_HASH_LITERAL "{digest}"

namespace raccoon::contract
{{
    // Contract hash of this raccoon-transport. Equal hash means equal message
    // layouts, channel names and ring layout.
    inline constexpr const char* kHash = "{digest}";

    // Fixed text that precedes the hash in the marker string embedded in every
    // binary, so the hash can be read from a built file without running it.
    inline constexpr const char* kMarkerPrefix = "{MARKER_PREFIX}";

    // Human-readable transport release. Set with
    // -DRACCOON_TRANSPORT_VERSION=... when building; "dev" otherwise.
    const char* version() noexcept;
}}
"""


def render_python(digest: str) -> str:
    return f'''"""{HEADER}"""

CONTRACT_HASH = "{digest}"
CONTRACT_MARKER_PREFIX = "{MARKER_PREFIX}"
# Embedded as a plain string so a built wheel can be scanned for it.
CONTRACT_MARKER = "{MARKER_PREFIX}{digest}"
'''


def render_dart(digest: str) -> str:
    return f"""// {HEADER}

/// Contract hash of this raccoon-transport. Equal hash means equal message
/// layouts, channel names and ring layout.
const String raccoonContractHash = '{digest}';

/// Marker string a build scan can search for in a compiled binary.
const String raccoonContractMarker = '{MARKER_PREFIX}{digest}';
"""


def generated_files(contract: dict) -> dict[Path, str]:
    digest = contract_hash(contract)
    return {
        CONTRACT_JSON: render_contract_json(contract, digest),
        CPP_OUT: render_cpp(digest),
        PY_OUT: render_python(digest),
        DART_OUT: render_dart(digest),
    }


# ── Drift between the languages ──────────────────────────────────────────


def _camel(snake: str) -> str:
    return "".join(part.capitalize() for part in snake.split("_"))


def _lower_camel(snake: str) -> str:
    head, *rest = snake.split("_")
    return head + "".join(part.capitalize() for part in rest)


def python_fields(path: Path) -> list[str] | None:
    """Field names of the dataclass in a generated Python type, in order."""
    import ast

    tree = ast.parse(path.read_text())
    for node in tree.body:
        if isinstance(node, ast.ClassDef) and node.name == path.stem:
            return [
                stmt.target.id
                for stmt in node.body
                if isinstance(stmt, ast.AnnAssign) and isinstance(stmt.target, ast.Name)
            ]
    return None


def dart_fields(path: Path, snake: str) -> list[str] | None:
    """Field names of the Dart class, in declaration order."""
    text = path.read_text()
    match = re.search(rf"class\s+{_camel(snake)}\s+implements\s+\w+\s*\{{(.*?)\n\s*\w+\(\{{", text, flags=re.S)
    if not match:
        return None
    body = re.sub(r"//[^\n]*", "", match.group(1))
    fields = []
    for line in body.split(";"):
        decl = re.fullmatch(r"\s*(?:final\s+)?[\w<>?, ]+?\s+(\w+)\s*", line)
        if decl:
            fields.append(decl.group(1))
    return fields


def check_drift(contract: dict) -> list[str]:
    problems: list[str] = []
    for name, fields in contract["types"].items():
        expected = [f[0] for f in fields]
        py_path = PY_TYPES_DIR / f"{name}.py"
        if not py_path.exists():
            problems.append(f"python: {name}.py is missing")
        else:
            found = python_fields(py_path)
            if found != expected:
                problems.append(f"python: {name} fields {found} != C++ {expected}")
        dart_path = next(DART_TYPES_DIR.rglob(f"{name}.g.dart"), None)
        if dart_path is None:
            problems.append(f"dart: {name}.g.dart is missing")
        else:
            found = dart_fields(dart_path, name)
            want = expected  # Dart keeps the snake_case field names
            if found != want:
                problems.append(f"dart: {name} fields {found} != C++ {want}")
    return problems


# ── Commands ─────────────────────────────────────────────────────────────


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--write", action="store_true")
    group.add_argument("--check", action="store_true")
    group.add_argument("--print", action="store_true", dest="show")
    args = parser.parse_args(argv)

    try:
        contract = build_contract()
    except ContractError as error:
        print(f"contract: {error}", file=sys.stderr)
        return 2
    digest = contract_hash(contract)

    if args.show:
        print(digest)
        return 0

    files = generated_files(contract)
    if args.write:
        for path, content in files.items():
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        print(f"contract hash {digest}")
        return 0

    stale = [p.relative_to(ROOT) for p, c in files.items() if not p.exists() or p.read_text() != c]
    drift = check_drift(contract)
    for path in stale:
        print(f"contract: {path} is out of date (run scripts/contract.py --write)", file=sys.stderr)
    for problem in drift:
        print(f"contract: drift: {problem}", file=sys.stderr)
    if stale or drift:
        return 1
    print(f"contract ok ({digest})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
