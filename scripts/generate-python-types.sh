#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
OUT_DIR="$ROOT_DIR/python/raccoon_transport/types"

# Clean existing generated files
rm -rf "$OUT_DIR/exlcm"

# Generate from all .lcm files
find "$ROOT_DIR/messages" -name "*.lcm" -exec lcm-gen -p --ppath "$OUT_DIR" {} \;

echo "Python types generated in $OUT_DIR"
