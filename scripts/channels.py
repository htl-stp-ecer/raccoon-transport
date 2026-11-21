#!/usr/bin/env python3
"""Read channels.yml, check it and generate the C++ channel table.

channels.yml is the one list of every channel: its name, message type and how its
ring behaves (see the comment at the top of that file). This script

* reads it (a small parser for the subset of YAML the file uses, so that no package
  is needed to run it),
* expands the port ranges,
* checks names, types, sizes and flags,
* computes the size of every ring slot from the message type, and
* writes cpp/include/raccoon/ChannelTable.h.

Usage:
    scripts/channels.py --write   regenerate the generated files
    scripts/channels.py --check   fail if a generated file is out of date or the table is invalid
    scripts/channels.py --list    print every channel with its slot size
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "channels.yml"
CONTRACT = ROOT / "contract" / "contract.json"
CPP_HEADER = ROOT / "cpp" / "include" / "raccoon" / "ChannelTable.h"

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
class PortRange:
    name: str             # the name with "{port}"
    first_index: int      # index of port `low` in the channel list
    low: int
    high: int


def parse_size(value, where: str) -> int:
    match = re.fullmatch(r"(\d+)(B|KiB|MiB)", str(value))
    if not match:
        raise TableError(f"{where}: max_payload must look like 256B, 64KiB or 4MiB, got {value!r}")
    return int(match.group(1)) * {"B": 1, "KiB": 1024, "MiB": 1024 * 1024}[match.group(2)]


def slot_stride(payload: int) -> int:
    """Distance between two slots: slot header plus payload, rounded up to a cache line."""
    return (SLOT_HEADER + payload + ALIGN - 1) // ALIGN * ALIGN


def expand(document: dict, sizes: dict[str, int | None]) -> tuple[list[Channel], list[PortRange]]:
    if document.get("format") != 1:
        raise TableError(f"{TABLE.name}: unsupported format {document.get('format')!r}")
    channels: list[Channel] = []
    ranges: list[PortRange] = []
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
            if "{port}" not in entry["name"]:
                raise TableError(f"{where}: ports without {{port}} in the name")
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

        if low is not None:
            ranges.append(PortRange(entry["name"], len(channels), low, high))
        for offset, concrete in enumerate(names):
            if not NAME_GRAMMAR.match(concrete):
                raise TableError(f"{where}: {concrete!r} is not a valid channel name")
            if concrete in seen:
                raise TableError(f"{where}: {concrete!r} is listed twice")
            seen.add(concrete)
            channels.append(Channel(concrete, message_type, entry["doc"], reliable, retained,
                                    slots, payload, size is not None, index,
                                    None if low is None else low + offset))
    return channels, ranges


def load() -> tuple[list[Channel], list[PortRange]]:
    document = parse_table(TABLE.read_text(encoding="utf-8"))
    contract = json.loads(CONTRACT.read_text(encoding="utf-8"))
    return expand(document, type_sizes(contract))


# ── C++ ────────────────────────────────────────────────────────────────────

def constant_name(name: str) -> str:
    return re.sub(r"[^0-9A-Za-z]", "_", name).upper()


def function_name(pattern: str) -> str:
    return re.sub(r"[^0-9A-Za-z]+", "_", pattern.replace("{port}", "")).strip("_").lower()


def cpp_header(channels: list[Channel], ranges: list[PortRange]) -> str:
    in_range = {c.name for r in ranges for c in channels[r.first_index:r.first_index + r.high - r.low + 1]}
    lines = [
        "// Generated by scripts/channels.py from channels.yml. Do not edit.",
        "//",
        "// Every channel of the transport with the properties of its ring. The slot",
        "// payload of a fixed-size type is the encoded size of the type, for the other",
        "// types it is the limit from the table.",
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
        "    // Channels without a port.",
    ]
    for channel in channels:
        if channel.name not in in_range:
            lines.append(f'    inline constexpr std::string_view {constant_name(channel.name)} = "{channel.name}";')
    lines += ["", "    // Channels of a port range: the name for a port, or an empty view when the port is out of range."]
    for r in ranges:
        function = function_name(r.name)
        lines.append(
            f"    constexpr std::string_view {function}(int port) noexcept")
        lines.append(
            f"    {{ return port < {r.low} || port > {r.high} ? std::string_view{{}} : "
            f"CHANNEL_TABLE[{r.first_index} + (port - {r.low})].name; }}")
    lines += ["}", ""]
    return "\n".join(lines)


GENERATED = [(CPP_HEADER, cpp_header)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--list", action="store_true")
    args = parser.parse_args()
    try:
        channels, ranges = load()
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
        content = render(channels, ranges)
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
