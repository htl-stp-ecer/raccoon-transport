<div align="center">

<img src="https://raw.githubusercontent.com/htl-stp-ecer/.github/main/profile/raccoon-logo.svg" alt="raccoon-transport" width="100"/>

# raccoon-transport

**The shared message and transport layer for RaccoonOS — LCM messaging across C++, Python, and Dart.**

[![CI](https://github.com/htl-stp-ecer/raccoon-transport/actions/workflows/ci.yml/badge.svg)](https://github.com/htl-stp-ecer/raccoon-transport/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/htl-stp-ecer/raccoon-transport)](https://github.com/htl-stp-ecer/raccoon-transport/releases/latest)
[![PyPI](https://img.shields.io/pypi/v/raccoon-transport?logo=pypi&logoColor=white)](https://pypi.org/project/raccoon-transport/)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](COPYING)
![C++20](https://img.shields.io/badge/C%2B%2B20-00599C?logo=cplusplus&logoColor=white)
![Python](https://img.shields.io/badge/Python-3.8+-3776AB?logo=python&logoColor=ffdd54)
![Dart](https://img.shields.io/badge/Dart-02569B?logo=dart&logoColor=white)

> 📖 **Full documentation at [raccoon-docs.pages.dev](https://raccoon-docs.pages.dev/)**

</div>

---

`raccoon-transport` is the message and transport layer shared across the robotics stack. It combines four concerns in one repo:

- the authoritative C++-side `.lcm` message definitions under `messages/`
- a C++ transport wrapper around LCM with retain and reliable-delivery support
- a thin Python transport package plus generated Python message types
- a Dart package that includes its own LCM runtime, parser, generator, and generated message set

This package is best treated as a small monorepo rather than a single library.

---

## Repository Layout

```text
raccoon-transport/
├── messages/            # Source .lcm files used by the C++ build
├── cpp/                 # C++ transport library and interop tools
├── python/              # Python package and pre-generated message classes
├── dart/                # Dart transport package, parser, generator, generated messages
├── integration_tests/   # Cross-language compatibility and stress tests
└── shared/              # Shared channel definitions
```

---

## Installation

### Python

```bash
pip install raccoon-transport
```

### C++ (via CMake FetchContent)

```cmake
FetchContent_Declare(raccoon_transport GIT_REPOSITORY https://github.com/htl-stp-ecer/raccoon-transport.git GIT_TAG main)
FetchContent_MakeAvailable(raccoon_transport)
target_link_libraries(my_target PRIVATE raccoon::transport)
```

### Dart (pubspec.yaml)

```yaml
dependencies:
  raccoon_transport:
    path: ../raccoon-transport/dart
```

---

## Quick Start (Python)

```python
from raccoon_transport import Transport, Channels
from raccoon_transport.types.raccoon import scalar_i32_t

t = Transport()

# Subscribe to sensor data
def on_motor_position(channel, data):
    msg = scalar_i32_t.decode(data)
    print(f"Motor position: {msg.value}")

t.subscribe(Channels.motor_position(0), on_motor_position)

# Publish a command
cmd = scalar_i32_t()
cmd.value = 50
t.publish(Channels.motor_power_command(0), cmd.encode())
```

---

## Building from Source

### C++

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j$(nproc)
```

### Dart

```bash
cd dart
dart pub get
dart run build_runner build --delete-conflicting-outputs
```

### Python (editable install)

```bash
pip install -e python/
```

### Regenerate Python LCM types

```bash
./scripts/generate-python-types.sh
```

---

## Integration Tests

32 tests covering all 6 directional language pairs (Python, Dart, C++):

```bash
PYTHONPATH=. pytest integration_tests/ -v

# Filter by suite
PYTHONPATH=. pytest integration_tests/test_cross_pubsub.py -v
PYTHONPATH=. pytest integration_tests/test_retain.py -v
PYTHONPATH=. pytest integration_tests/test_stress.py -v
```

---

## Architecture

### Transport Layer

- **Plain publish/subscribe** — direct LCM call, zero overhead
- **Reliable delivery** — at-least-once with acknowledgement protocol via `__raccoon/` control channels
- **Retain** — latest-value-on-subscribe, so new subscribers immediately get the most recent message

### Channel Naming

All channels follow the pattern `raccoon/<device>/<property>`. Protocol channels use the `__raccoon/` prefix.

### Message Types

Defined in `messages/types/*.lcm` under the `raccoon` package:

| Type | Description |
|:-----|:------------|
| `vector3f_t` | 3D float vector (gyro, accel, mag) |
| `quaternion_t` | Orientation quaternion |
| `scalar_f_t` | Float scalar |
| `scalar_i32_t` | 32-bit integer scalar |
| `scalar_i8_t` | 8-bit integer scalar |
| `string_t` | String message |
| `yolo_frame_t` | Camera frame with detection data |
| `yolo_box_t` | Single detection bounding box |

### Language-Specific Notes

- **C++**: Messages auto-generated from `messages/`. Transport uses PIMPL pattern.
- **Dart**: Keeps its own local `.lcm` copies under `dart/lib/messages/`. Includes a full LCM parser and Dart code generator. Run `build_runner` after editing `.lcm` files.
- **Python**: Pre-generated types in `python/raccoon_transport/types/`. Regenerate with `./scripts/generate-python-types.sh`.

---

## Part of RaccoonOS

| Repository | What it is |
|:-----------|:-----------|
| [raccoon-lib](https://github.com/htl-stp-ecer/raccoon-lib) | Core robotics library — PID, kinematics, step-based missions |
| [stm32-data-reader](https://github.com/htl-stp-ecer/stm32-data-reader) | Raspberry Pi ↔ STM32 SPI bridge |
| [raccoon-cli](https://github.com/htl-stp-ecer/raccoon-cli) | Dev toolchain — scaffolding, codegen, remote sync |
| [botui](https://github.com/htl-stp-ecer/botui) | Flutter desktop environment |
| [documentation](https://raccoon-docs.pages.dev/) | Full platform docs |

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to add message types, run tests, and submit changes.

---

## License

Copyright (C) 2026 Tobias Madlberger
Licensed under the GNU General Public License v3.0 — see [COPYING](COPYING) for details.
