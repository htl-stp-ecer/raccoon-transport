#!/usr/bin/env python3
"""Read channels.yml, check it and generate the channel table in C++, Python and Dart.

channels.yml is the one list of every channel: its name, message type and how its
ring behaves (see the comment at the top of that file). This script

* reads it (a small parser for the subset of YAML the file uses, so that no package
  is needed to run it),
* expands the port ranges,
* checks names, types, sizes and flags,
* computes the size of every ring slot from the message type, and
* writes cpp/include/raccoon/ChannelTable.h, python/raccoon_transport/channel_table.py and
  dart/lib/src/channel_table.dart. Each of them names a channel by its path, for example
  channels::hardware::motor::cmd::power(port) in C++ and channels.hardware.motor.cmd.power(port)
  in Python and Dart.

Usage:
    scripts/channels.py --write   regenerate the generated files
    scripts/channels.py --check   fail if a generated file is out of date or the table is invalid
    scripts/channels.py --list    print every channel with its slot size
"""
from __future__ import annotations

import argparse
import json
import keyword
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "channels.yml"
CONTRACT = ROOT / "contract" / "contract.json"
CPP_HEADER = ROOT / "cpp" / "include" / "raccoon" / "ChannelTable.h"
PY_MODULE = ROOT / "python" / "raccoon_transport" / "channel_table.py"
DART_MODULE = ROOT / "dart" / "lib" / "src" / "channel_table.dart"

NAME_GRAMMAR = re.compile(r"^[0-9A-Za-z_-]+(/[0-9A-Za-z_-]+)*$")
ENTRY_KEYS = {"name", "ports", "type", "doc", "reliable", "retained", "slots", "max_payload"}
DEFAULT_SLOTS = 64
MIN_SLOTS, MAX_SLOTS = 2, 4096
MAX_PAYLOAD_LIMIT = 1024 * 1024 * 1024
SLOT_HEADER = 16
ALIGN = 64

FIELD_SIZES = {"i8": 1, "i16": 2, "i32": 4, "i64": 8, "f32": 4, "f64": 8, "bool": 1}


class TableError(Exception):
    pass


# ── Reading channels.yml ───────────────────────────────────────────────────

def _split_top_level(text: str, separator: str) -> list[str]:
    parts, current, in_quote, escaped = [], [], False, False
    for char in text:
        if escaped:
            current.append(char)
            escaped = False
        elif char == "\\" and in_quote:
            current.append(char)
            escaped = True
        elif char == '"':
            in_quote = not in_quote
            current.append(char)
        elif char == separator and not in_quote:
            parts.append("".join(current))
            current = []
        else:
            current.append(char)
    parts.append("".join(current))
    return parts


def _scalar(token: str):
    token = token.strip()
    if len(token) >= 2 and token[0] == '"' and token[-1] == '"':
        return re.sub(r'\\(["\\])', r"\1", token[1:-1])
    if token in ("true", "false"):
        return token == "true"
    if re.fullmatch(r"-?\d+", token):
        return int(token)
    return token


def parse_table(text: str) -> dict:
    """Parse channels.yml: `format: N` and a `channels:` list of one-line flow mappings."""
    document: dict = {"channels": []}
    in_channels = False
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        try:
            if raw.startswith("channels:"):
                in_channels = True
            elif not raw.startswith((" ", "\t")):
                key, _, value = line.partition(":")
                document[key.strip()] = _scalar(value)
            elif in_channels and line.startswith("- {") and line.endswith("}"):
                entry = {}
                for pair in _split_top_level(line[3:-1], ","):
                    if not pair.strip():
                        continue
                    key, separator, value = pair.partition(":")
                    if not separator:
                        raise TableError(f"expected 'key: value', got {pair.strip()!r}")
                    entry[_scalar(key)] = _scalar(value)
                document["channels"].append(entry)
            else:
                raise TableError(f"cannot read {line!r}")
        except TableError as error:
            raise TableError(f"{TABLE.name}:{number}: {error}") from None
    return document


# ── Types ──────────────────────────────────────────────────────────────────

def fixed_size(field_type: str) -> int | None:
    """Encoded size of a field type in bytes, or None when it has no fixed size."""
    if field_type in FIELD_SIZES:
        return FIELD_SIZES[field_type]
    array = re.fullmatch(r"array<(\w+),(\d+)>", field_type)
    if array and array.group(1) in FIELD_SIZES:
        return FIELD_SIZES[array.group(1)] * int(array.group(2))
    return None


def type_sizes(contract: dict) -> dict[str, int | None]:
    """Message type -> fixed encoded size in bytes, or None for a type without one."""
    sizes: dict[str, int | None] = {}
    for name, fields in contract["types"].items():
        total: int | None = 0
        for _, field_type in fields:
            size = fixed_size(field_type)
            if size is None:
                total = None
                break
            total += size
        sizes[name] = total
    return sizes


# ── Channels ───────────────────────────────────────────────────────────────

@dataclass(frozen=True)
class Channel:
    name: str
    type: str
    doc: str
    reliable: bool
    retained: bool
    slots: int
    payload: int          # bytes of one frame: the exact size of a fixed type, else the limit
    fixed: bool
    entry: int            # index of the table entry the channel comes from
    port: int | None      # port number for a channel of a port range


@dataclass(frozen=True)
class Leaf:
    """One entry of the table: a single channel or a range of ports."""
    pattern: str                 # the name, with "{port}" for a range
    low: int | None              # first port of a range, None for a single channel
    high: int | None
    first_index: int             # index of the first channel in the channel list


def parse_size(value, where: str) -> int:
    match = re.fullmatch(r"(\d+)(B|KiB|MiB)", str(value))
    if not match:
        raise TableError(f"{where}: max_payload must look like 256B, 64KiB or 4MiB, got {value!r}")
    return int(match.group(1)) * {"B": 1, "KiB": 1024, "MiB": 1024 * 1024}[match.group(2)]


def slot_stride(payload: int) -> int:
    """Distance between two slots: slot header plus payload, rounded up to a cache line."""
    return (SLOT_HEADER + payload + ALIGN - 1) // ALIGN * ALIGN


def expand(document: dict, sizes: dict[str, int | None]) -> tuple[list[Channel], list[Leaf]]:
    if document.get("format") != 1:
        raise TableError(f"{TABLE.name}: unsupported format {document.get('format')!r}")
    channels: list[Channel] = []
    leaves: list[Leaf] = []
    seen: set[str] = set()
    for index, entry in enumerate(document["channels"]):
        name = entry.get("name", f"<entry {index}>")
        where = f"{TABLE.name}: {name}"
        unknown = set(entry) - ENTRY_KEYS
        if unknown:
            raise TableError(f"{where}: unknown field {sorted(unknown)[0]!r}")
        for required in ("name", "type", "doc"):
            if required not in entry:
                raise TableError(f"{where}: missing {required!r}")

        names = [entry["name"]]
        low = high = None
        if "ports" in entry:
            ports = re.fullmatch(r"(\d+)\.\.(\d+)", str(entry["ports"]))
            if not ports or int(ports.group(1)) > int(ports.group(2)):
                raise TableError(f"{where}: ports must look like 0..3")
            low, high = int(ports.group(1)), int(ports.group(2))
            if "{port}" not in entry["name"].split("/"):
                raise TableError(f"{where}: ports need {{port}} as a whole segment of the name")
            names = [entry["name"].replace("{port}", str(p)) for p in range(low, high + 1)]
        elif "{port}" in entry["name"]:
            raise TableError(f"{where}: {{port}} in the name without ports")

        message_type = entry["type"]
        if message_type not in sizes:
            raise TableError(f"{where}: unknown message type {message_type!r}")
        size = sizes[message_type]
        if size is None:
            if "max_payload" not in entry:
                raise TableError(f"{where}: {message_type} has no fixed size, max_payload is required")
            payload = parse_size(entry["max_payload"], where)
            if not 0 < payload <= MAX_PAYLOAD_LIMIT:
                raise TableError(f"{where}: max_payload out of range")
        else:
            if "max_payload" in entry:
                raise TableError(f"{where}: {message_type} has a fixed size of {size} bytes, max_payload must not be set")
            payload = size

        slots = entry.get("slots", DEFAULT_SLOTS)
        if not isinstance(slots, int) or not MIN_SLOTS <= slots <= MAX_SLOTS:
            raise TableError(f"{where}: slots must be a number from {MIN_SLOTS} to {MAX_SLOTS}")
        for flag in ("reliable", "retained"):
            if flag in entry and not isinstance(entry[flag], bool):
                raise TableError(f"{where}: {flag} must be true or false")
        retained = entry.get("retained", "state" in entry["name"].split("/"))
        reliable = entry.get("reliable", False)

        leaves.append(Leaf(entry["name"], low, high, len(channels)))
        for offset, concrete in enumerate(names):
            if not NAME_GRAMMAR.match(concrete):
                raise TableError(f"{where}: {concrete!r} is not a valid channel name")
            if concrete in seen:
                raise TableError(f"{where}: {concrete!r} is listed twice")
            seen.add(concrete)
            channels.append(Channel(concrete, message_type, entry["doc"], reliable, retained,
                                    slots, payload, size is not None, index,
                                    None if low is None else low + offset))
    return channels, leaves


def load() -> tuple[list[Channel], list[Leaf]]:
    document = parse_table(TABLE.read_text(encoding="utf-8"))
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    return expand(document, type_sizes(contract))


# ── Channel tree ───────────────────────────────────────────────────────────
#
# A channel is named by its path in every language: hardware/motor/{port}/cmd/power
# becomes channels::hardware::motor::cmd::power(port) in C++ and
# channels.hardware.motor.cmd.power(port) in Python and Dart. The segment "{port}" is
# not part of the path; the leaf is a function of the port instead.

CPP_RESERVED = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case", "catch", "char", "class", "concept",
    "const", "constexpr", "continue", "decltype", "default", "delete", "do", "double", "else", "enum", "explicit",
    "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int", "long", "mutable",
    "namespace", "new", "noexcept", "not", "nullptr", "operator", "or", "private", "protected", "public",
    "register", "requires", "return", "short", "signed", "sizeof", "static", "struct", "switch", "template",
    "this", "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
    "void", "volatile", "while", "xor",
}
DART_RESERVED = {
    "abstract", "as", "assert", "async", "await", "break", "case", "catch", "class", "const", "continue",
    "default", "do", "dynamic", "else", "enum", "export", "extends", "extension", "external", "factory", "false",
    "final", "finally", "for", "get", "if", "implements", "import", "in", "interface", "is", "late", "library",
    "mixin", "new", "null", "on", "operator", "part", "required", "rethrow", "return", "set", "static", "super",
    "switch", "this", "throw", "true", "try", "typedef", "var", "void", "while", "with", "yield",
}


class Node:
    def __init__(self) -> None:
        self.children: dict[str, "Node"] = {}
        self.leaf: Leaf | None = None


def identifier(segment: str, where: str) -> str:
    ident = re.sub(r"[^0-9A-Za-z]", "_", segment).lower()
    if ident[0].isdigit():
        raise TableError(f"{where}: segment {segment!r} starts with a digit")
    if ident in CPP_RESERVED or ident in DART_RESERVED or keyword.iskeyword(ident):
        raise TableError(f"{where}: segment {segment!r} is a reserved word in C++, Python or Dart")
    return ident


def camel(ident: str) -> str:
    head, *rest = ident.split("_")
    return head + "".join(part.capitalize() for part in rest)


def build_tree(leaves: list[Leaf]) -> Node:
    root = Node()
    for leaf in leaves:
        where = f"{TABLE.name}: {leaf.pattern}"
        node = root
        for segment in [s for s in leaf.pattern.split("/") if s != "{port}"]:
            name = identifier(segment, where)
            if node.leaf is not None:
                raise TableError(f"{where}: {segment!r} lies below another channel")
            node = node.children.setdefault(name, Node())
        if node.leaf is not None or node.children:
            raise TableError(f"{where}: the same path as another channel or a part of one")
        node.leaf = leaf
    check_names(root, "")
    return root


def check_names(node: Node, path: str) -> None:
    seen: dict[str, str] = {}
    for name in node.children:
        for variant in (name.replace("_", ""), camel(name)):
            if variant.lower() in seen and seen[variant.lower()] != name:
                raise TableError(f"{TABLE.name}: {path or '/'}: {name!r} and {seen[variant.lower()]!r} give the same name in a language")
            seen[variant.lower()] = name
    for name, child in node.children.items():
        check_names(child, f"{path}/{name}")


def walk(node: Node, path: tuple[str, ...] = ()):
    """Yield (path, node) for every node below `node`, in the order of the table."""
    for name, child in node.children.items():
        yield path + (name,), child
        yield from walk(child, path + (name,))


# ── C++ ────────────────────────────────────────────────────────────────────

def cpp_leaf(name: str, leaf: Leaf, indent: str) -> list[str]:
    if leaf.low is None:
        return [f'{indent}inline constexpr std::string_view {name.upper()} = "{leaf.pattern}";']
    return [
        f"{indent}constexpr std::string_view {name}(int port) noexcept",
        f"{indent}{{ return port < {leaf.low} || port > {leaf.high} ? std::string_view{{}} : "
        f"CHANNEL_TABLE[{leaf.first_index} + (port - {leaf.low})].name; }}",
    ]


def cpp_tree(node: Node, depth: int) -> list[str]:
    indent = "    " * depth
    lines: list[str] = []
    for name, child in node.children.items():
        if child.leaf is not None:
            lines += cpp_leaf(name, child.leaf, indent)
        else:
            lines.append(f"{indent}namespace {name}")
            lines.append(f"{indent}{{")
            lines += cpp_tree(child, depth + 1)
            lines.append(f"{indent}}}")
    return lines


def cpp_header(channels: list[Channel], leaves: list[Leaf]) -> str:
    tree = build_tree(leaves)
    lines = [
        "// Generated by scripts/channels.py from channels.yml. Do not edit.",
        "//",
        "// Every channel of the transport with the properties of its ring. The slot",
        "// payload of a fixed-size type is the encoded size of the type, for the other",
        "// types it is the limit from the table.",
        "//",
        "// A channel is named by its path: hardware/motor/{port}/cmd/power is",
        "// channels::hardware::motor::cmd::power(port), hardware/imu/state/gyro is",
        "// channels::hardware::imu::state::GYRO.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <string_view>",
        "",
        "namespace raccoon::channels",
        "{",
        "    struct ChannelInfo",
        "    {",
        "        std::string_view name;",
        "        std::string_view type;  // message type, for example \"scalar_i32_t\"",
        "        uint32_t slots;         // depth of the ring",
        "        uint32_t payload;       // bytes of one frame: exact for a fixed-size type, else the limit",
        "        bool fixed_size;",
        "        bool reliable;",
        "        bool retained;",
        "    };",
        "",
        f"    inline constexpr std::size_t CHANNEL_COUNT = {len(channels)};",
        "",
        "    inline constexpr ChannelInfo CHANNEL_TABLE[CHANNEL_COUNT] = {",
    ]
    for channel in channels:
        lines.append(
            f'        {{"{channel.name}", "{channel.type}", {channel.slots}, {channel.payload}, '
            f'{str(channel.fixed).lower()}, {str(channel.reliable).lower()}, {str(channel.retained).lower()}}},')
    lines += [
        "    };",
        "",
        "    // The table entry of a channel, or nullptr when the channel is not in the table.",
        "    constexpr const ChannelInfo* find_channel(std::string_view name) noexcept",
        "    {",
        "        for (const ChannelInfo& info : CHANNEL_TABLE)",
        "        {",
        "            if (info.name == name) return &info;",
        "        }",
        "        return nullptr;",
        "    }",
        "",
        "    // Distance between two slots of a ring: slot header plus payload, rounded up to a cache line.",
        "    constexpr uint64_t slot_stride(uint32_t payload) noexcept",
        f"    {{ return (uint64_t{{{SLOT_HEADER}}} + payload + {ALIGN - 1}) / {ALIGN} * {ALIGN}; }}",
        "",
        "    // The channels by path. A channel of a port range is a function of the port that",
        "    // returns the name, or an empty name for a port outside the range.",
    ]
    lines += cpp_tree(tree, 1)
    lines += ["}", ""]
    return "\n".join(lines)


# ── Python ─────────────────────────────────────────────────────────────────

def py_tree(node: Node, depth: int) -> list[str]:
    indent = "    " * depth
    lines: list[str] = []
    for name, child in node.children.items():
        if child.leaf is not None:
            leaf = child.leaf
            if leaf.low is None:
                lines.append(f'{indent}{name.upper()} = "{leaf.pattern}"')
            else:
                lines += [
                    f"{indent}@staticmethod",
                    f"{indent}def {name}(port: int) -> str:",
                    f'{indent}    return _port("{leaf.pattern}", port, {leaf.low}, {leaf.high})',
                    "",
                ]
        else:
            lines.append(f"{indent}class {name}:")
            lines += py_tree(child, depth + 1)
            lines.append("")
    return lines


def py_module(channels: list[Channel], leaves: list[Leaf]) -> str:
    tree = build_tree(leaves)
    lines = [
        '"""Generated by scripts/channels.py from channels.yml. Do not edit.',
        "",
        "A channel is named by its path: hardware/motor/{port}/cmd/power is",
        "channels.hardware.motor.cmd.power(port), hardware/imu/state/gyro is",
        'channels.hardware.imu.state.GYRO.',
        '"""',
        "",
        "# ruff: noqa",
        "from __future__ import annotations",
        "",
        "",
        "def _port(pattern: str, port: int, low: int, high: int) -> str:",
        "    if isinstance(port, bool) or not isinstance(port, int) or not low <= port <= high:",
        '        raise ValueError(f"port {port!r} is outside {low}..{high} for {pattern}")',
        '    return pattern.replace("{port}", str(port))',
        "",
        "",
        "class channels:",
    ]
    body = py_tree(tree, 1)
    while body and body[-1] == "":
        body.pop()
    lines += body
    lines += [""]
    return "\n".join(lines)


# ── Dart ───────────────────────────────────────────────────────────────────

def dart_class_name(path: tuple[str, ...]) -> str:
    return "_" + "".join(camel(part)[0].upper() + camel(part)[1:] for part in path)


def dart_tree(node: Node, path: tuple[str, ...]) -> list[str]:
    name = dart_class_name(path) if path else "_Channels"
    members, nested = [], []
    for child_name, child in node.children.items():
        member = camel(child_name)
        if child.leaf is not None:
            leaf = child.leaf
            if leaf.low is None:
                members.append(f"  String get {member} => '{leaf.pattern}';")
            else:
                members.append(f"  String {member}(int port) => _port('{leaf.pattern}', port, {leaf.low}, {leaf.high});")
        else:
            members.append(f"  final {member} = const {dart_class_name(path + (child_name,))}();")
            nested += dart_tree(child, path + (child_name,))
    lines = [f"class {name} {{", f"  const {name}();"]
    lines += members
    lines += ["}", ""]
    return lines + nested


def dart_module(channels: list[Channel], leaves: list[Leaf]) -> str:
    tree = build_tree(leaves)
    lines = [
        "// Generated by scripts/channels.py from channels.yml. Do not edit.",
        "//",
        "// A channel is named by its path: hardware/motor/{port}/cmd/power is",
        "// channels.hardware.motor.cmd.power(port), hardware/imu/state/gyro is",
        "// channels.hardware.imu.state.gyro.",
        "",
        "String _port(String pattern, int port, int low, int high) {",
        "  if (port < low || port > high) {",
        "    throw RangeError.range(port, low, high, 'port', pattern);",
        "  }",
        "  return pattern.replaceFirst('{port}', port.toString());",
        "}",
        "",
        "const channels = _Channels();",
        "",
    ]
    lines += dart_tree(tree, ())
    while lines and lines[-1] == "":
        lines.pop()
    lines.append("")
    return "\n".join(lines)


GENERATED = [(CPP_HEADER, cpp_header), (PY_MODULE, py_module), (DART_MODULE, dart_module)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--list", action="store_true")
    args = parser.parse_args()
    try:
        channels, leaves = load()
    except TableError as error:
        print(f"channels: {error}", file=sys.stderr)
        return 1

    if args.list:
        for channel in channels:
            print(f"{channel.name:62} {channel.type:24} slots={channel.slots:<4} payload={channel.payload:<9}"
                  f" stride={slot_stride(channel.payload):<9}{'reliable ' if channel.reliable else ''}"
                  f"{'retained' if channel.retained else ''}")
        return 0

    stale = []
    for path, render in GENERATED:
        content = render(channels, leaves)
        if args.write:
            path.write_text(content, encoding="utf-8")
        elif not path.exists() or path.read_text(encoding="utf-8") != content:
            stale.append(path.relative_to(ROOT))
    if stale:
        print("channels: out of date, run scripts/channels.py --write: " + ", ".join(map(str, stale)), file=sys.stderr)
        return 1
    print(f"channels ok ({len(channels)} channels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
