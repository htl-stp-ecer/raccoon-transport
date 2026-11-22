#!/usr/bin/env bash
# Cross-compile libraccoon_transport_bridge.so for aarch64 (Pi 3/4/5).
#
# The bridge is the C interface of raccoon::Transport (raccoon_transport_bridge.h). It
# exists so that Flutter/Dart consumers use the same transport as the C++ and Python
# sides, through dart:ffi.
#
# Output:  libraccoon_transport_bridge.so (the transport, the ring and the message
# types are linked in; it only needs libc/libstdc++/libgcc_s at runtime).
#
# Override the toolchain with CC_CROSS / CXX env vars if you're on
# something other than Debian-style aarch64 cross packages.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

ROOT="$SCRIPT_DIR/../.."
INC="$ROOT/cpp/include"
MSG_INC="$ROOT/messages/include"

CXX="${CXX:-aarch64-linux-gnu-g++}"
CC_CROSS="${CC_CROSS:-aarch64-linux-gnu-gcc}"

echo "▶ Building libraccoon_transport_bridge.so with $CXX"

OBJ_DIR="$(mktemp -d)"
trap 'rm -rf "$OBJ_DIR"' EXIT

"$CC_CROSS" -std=c11 -fPIC -O2 -Wall -Wextra -I"$INC" \
  -c "$ROOT/cpp/src/raccoon_ring.c" -o "$OBJ_DIR/raccoon_ring.o"

"$CXX" -std=c++20 -fPIC -O2 -Wall -Wextra -fvisibility=hidden \
  -I"$SCRIPT_DIR" -I"$INC" -I"$MSG_INC" \
  -shared raccoon_transport_bridge.cpp "$ROOT/cpp/src/Transport.cpp" \
  "$ROOT"/messages/*/*.cpp "$OBJ_DIR/raccoon_ring.o" \
  -lpthread \
  -o libraccoon_transport_bridge.so

echo "  built $(file libraccoon_transport_bridge.so | cut -d, -f1-3)"
echo "  size:   $(stat -c%s libraccoon_transport_bridge.so) bytes"
echo "  NEEDED: $(readelf -d libraccoon_transport_bridge.so | awk '/NEEDED/ {print $5}' | xargs)"
