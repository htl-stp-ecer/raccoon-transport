<div align="center">

<img src="https://raw.githubusercontent.com/htl-stp-ecer/.github/main/profile/raccoon-logo.svg" alt="raccoon-transport" width="100"/>

# raccoon-transport

**The shared message and transport layer for RaccoonOS — shared-memory messaging across C++, Python, and Dart.**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](COPYING)
![C++20](https://img.shields.io/badge/C%2B%2B20-00599C?logo=cplusplus&logoColor=white)
![Python](https://img.shields.io/badge/Python-3.8+-3776AB?logo=python&logoColor=ffdd54)
![Dart](https://img.shields.io/badge/Dart-02569B?logo=dart&logoColor=white)
[![CI](https://github.com/htl-stp-ecer/raccoon-transport/actions/workflows/ci.yml/badge.svg)](https://github.com/htl-stp-ecer/raccoon-transport/actions/workflows/ci.yml)
[![Latest Release](https://img.shields.io/github/v/release/htl-stp-ecer/raccoon-transport)](https://github.com/htl-stp-ecer/raccoon-transport/releases/latest)
[![PyPI](https://img.shields.io/pypi/v/raccoon-transport?logo=pypi&logoColor=white)](https://pypi.org/project/raccoon-transport/)

> 📖 **Full documentation at [raccoon-docs.pages.dev](https://raccoon-docs.pages.dev/)**

</div>

---

`raccoon-transport` is the message and transport layer shared across the robotics stack. It combines four concerns in one repo:

- the message definitions, written by hand in C++ under `messages/`
- a C++ transport library with retain and reliable-delivery support
- a Python transport package with matching message classes
- a Dart package with its own transport and matching message classes
- a contract hash that lets every component check at startup that it speaks the same wire format

This package is best treated as a small monorepo rather than a single library.

---

## Repository Layout

```text
raccoon-transport/
├── messages/            # Message structs (C++ is the source of truth) and protocol types
├── cpp/                 # C++ transport library, channels, ring bridge, tests
├── python/              # Python package and message classes
├── dart/                # Dart transport package and message classes
├── contract/            # Contract hash, startup-check spec and test vectors
├── scripts/             # contract.py (hash generator and check)
├── integration_tests/   # Cross-language compatibility and stress tests
├── build.py             # Builds the Python wheel and contract assets in Docker
└── deploy.py            # Installs a local build on a Pi
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

### Python wheel and contract assets (Docker)

```bash
python3 build.py                 # result in builds/, version 0.0
python3 deploy.py --host <pi>    # install it on a Pi for development
```

Needs only Docker and Python 3. Writes the wheel, `contract-hash.txt` and `contract.json` to `builds/`.

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
```

### Python (editable install)

```bash
pip install -e python/
```

### Contract hash

After changing a message struct, a channel or the ring layout:

```bash
python3 scripts/contract.py --write   # regenerate contract.json and the constants for C++, Python and Dart
python3 scripts/contract.py --check   # fails if a generated file is stale or the languages drifted apart (also runs in CI)
```

---

## Integration Tests

Cross-language tests between Python, Dart and C++:

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

- **Plain publish/subscribe** — one file per channel under `/dev/shm/` (`raccoon_ring`, a single-producer multi-consumer ring buffer), no daemon
- **Reliable delivery** — at-least-once with acknowledgement protocol via `__raccoon/` control channels
- **Retain** — latest-value-on-subscribe, so new subscribers immediately get the most recent message

### Channel Naming

All channels follow the pattern `raccoon/<device>/<property>`. Protocol channels use the `__raccoon/` prefix.

### Message Types

Defined in `messages/types/` under the `raccoon` namespace. The wire format is written by hand in each language, and C++ is the source of truth.

| Type | Description |
|:-----|:------------|
| `scalar_f_t`, `scalar_i32_t`, `scalar_i8_t` | Float / 32-bit / 8-bit integer scalar |
| `vector3f_t` | 3D float vector (gyro, accel, mag) |
| `quaternion_t` | Orientation quaternion |
| `orientation_matrix_t` | 3×3 orientation matrix |
| `string_t` | String message |
| `kinematics_config_t` | Kinematics matrices and motor calibration |
| `screen_render_t`, `screen_render_answer_t` | Screen request from the robot and the answer from botui |
| `cam_frame_t`, `cam_detections_t`, `cam_blob_t` | Camera frame and detections (raccoon-cam) |
| `cam_config_t`, `cam_stream_ctl_t` | Camera config and stream on/off |
| `yolo_frame_t`, `yolo_box_t` | Frame with YOLO boxes (`raccoon/yolo/frame`) |
| `ack_t`, `envelope_t`, `retain_request_t` | Protocol types for reliable delivery and retain |

### Language-Specific Notes

- **C++**: Message structs live in `messages/`, the transport library in `cpp/`. Transport uses the PIMPL pattern.
- **Dart**: Has its own transport and message classes under `dart/lib/`. No code generation step.
- **Python**: Message classes in `python/raccoon_transport/types/raccoon/`, transport in `python/raccoon_transport/`.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to add message types, run tests, and submit changes.

---

## License

Copyright (C) 2026 Tobias Madlberger
Licensed under the GNU General Public License v3.0 — see [COPYING](COPYING) for details.
