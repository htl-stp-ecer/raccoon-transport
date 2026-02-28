# CLAUDE.md

## Project Overview

raccoon-transport is a shared LCM (Lightweight Communications and Marshalling) transport library used across three robotics projects. It provides:
1. All LCM message type definitions (replacing the `lcm-messages` git submodule)
2. Channel definitions in C++, Dart, and Python
3. A transport layer with optional at-least-once reliable delivery and retain (latest-value-on-subscribe)

## Build Commands

### C++ (via CMake):
```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j$(nproc)
```

### Dart:
```bash
cd dart
dart pub get
dart run build_runner build  # Generate .g.dart from .lcm files
```

### Python:
```bash
cd python
pip install -e .
```

### Regenerate Python LCM types:
```bash
./scripts/generate-python-types.sh
```

## Architecture

### Message Types
- `messages/types/` — 11 existing `exlcm::` message types (preserved namespace)
- `messages/protocol/` — `raccoon::` protocol types for reliability/retain

### C++ Library (`cpp/`)
- `raccoon::Transport` — Main API (PIMPL pattern), wraps `lcm::LCM`
- `raccoon::Channels` — All channel name definitions
- `raccoon::LcmMessage` concept — Type constraint for LCM messages
- Plain publish/subscribe = direct LCM call (zero overhead)
- Reliable/retain features use `__raccoon/` control channels

### Dart Package (`dart/`)
- `RaccoonTransport` — Wraps the `Lcm` UDP client
- Includes full LCM parser + Dart code generator (absorbed from lcm_dart)
- `build_runner` generates `.g.dart` from `.lcm` files

### Python Package (`python/`)
- `Transport` — Wraps `lcm.LCM`
- Pre-generated Python types in `raccoon_transport/types/exlcm/`

### Channel Naming
All channels follow pattern `libstp/<device>/<property>`.
Protocol channels use `__raccoon/` prefix.

## Consumer Integration

### CMake (C++):
```cmake
FetchContent_Declare(raccoon_transport GIT_REPOSITORY <url> GIT_TAG main)
FetchContent_MakeAvailable(raccoon_transport)
target_link_libraries(my_target PRIVATE raccoon::transport)
```

### Dart (pubspec.yaml):
```yaml
dependencies:
  raccoon_transport:
    path: ../raccoon-transport/dart
```

### Python:
```bash
pip install -e ../raccoon-transport/python
```
