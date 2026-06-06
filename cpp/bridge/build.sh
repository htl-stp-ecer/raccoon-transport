#!/usr/bin/env bash
# Cross-compile libraccoon_ring_bridge.so for aarch64 (Pi 3/4/5).
#
# The bridge is a thin C++ Dart-FFI wrapper around raccoon_ring (in
# ../src/raccoon_ring.c). It exists so Flutter/Dart consumers can talk
# to the same SHM ring buffers that the C++/Python sides of
# raccoon-transport use, without depending on the rest of the C++
# Transport class.
#
# Output:  libraccoon_ring_bridge.so (statically embeds raccoon_ring,
# only needs libc/libstdc++/libgcc_s at runtime).
#
# Override the toolchain with CC_CROSS / CXX env vars if you're on
# something other than Debian-style aarch64 cross packages.

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

RRB_ROOT="$SCRIPT_DIR/.."
RRB_INC="$RRB_ROOT/include"
RRB_SRC="$RRB_ROOT/src/raccoon_ring.c"

if [ ! -f "$RRB_SRC" ] || [ ! -f "$RRB_INC/raccoon/raccoon_ring.h" ]; then
  echo "ERROR: raccoon_ring source not found at $RRB_ROOT" >&2
  exit 1
fi

CXX="${CXX:-aarch64-linux-gnu-g++}"
CC_CROSS="${CC_CROSS:-aarch64-linux-gnu-gcc}"

echo "▶ Building libraccoon_ring_bridge.so with $CXX"

"$CC_CROSS" -std=c11 -fPIC -O2 -Wall -Wextra \
  -I"$RRB_INC" \
  -c "$RRB_SRC" -o raccoon_ring.o

"$CXX" -std=c++17 -fPIC -O2 -Wall -Wextra \
  -I"$SCRIPT_DIR" -I"$RRB_INC" \
  -shared raccoon_ring_bridge.cpp raccoon_ring.o \
  -lpthread \
  -o libraccoon_ring_bridge.so

rm -f raccoon_ring.o

echo "  built $(file libraccoon_ring_bridge.so | cut -d, -f1-3)"
echo "  size:   $(stat -c%s libraccoon_ring_bridge.so) bytes"
echo "  NEEDED: $(readelf -d libraccoon_ring_bridge.so | awk '/NEEDED/ {print $5}' | xargs)"
