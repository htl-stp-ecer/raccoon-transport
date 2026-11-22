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
- a Python package with matching message classes and channel names
- a Dart package with matching message classes and access to the shared-memory rings through a C bridge
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

`raccoon_transport.Transport` is a thin wrapper around the C++ transport: the rings, reliable delivery, retained channels and statistics are the C++ implementation. A channel is named by its path in the channel table, and the message type of a channel is checked on publish and subscribe:

```python
import raccoon_transport as rt
from raccoon_transport import channels
from raccoon_transport.types.raccoon import scalar_i32_t

transport = rt.get_transport()              # one per process, shared

# publish a typed message; a timestamp of 0 is stamped
transport.publish(channels.hardware.motor.cmd.power(0), scalar_i32_t(timestamp=0, value=50))

# subscribe: the handler gets (channel, message) on the transport's thread
sub = transport.subscribe(channels.hardware.motor.state.position(0),
                          lambda channel, msg: print(msg.value),
                          message_type=scalar_i32_t)
transport.unsubscribe(sub)
```

Without `message_type` the handler receives the payload bytes. `publish_raw(channel, payload)` sends bytes. A publish that is refused raises `PublishError`; its `result` is a `PublishResult` (`NotInTable`, `TypeMismatch`, `TooBig` or `Closed`). Whether a channel is reliable or retained is a property of the channel table.

A pure install (without the native module, for a laptop) has the message classes, the channel names and the contract check; `Transport` raises when it is created.

---

## Building from Source

### Python wheel and contract assets (Docker)

```bash
python3 build.py                 # result in builds/, version 0.0
python3 deploy.py --host <pi>    # install it on a Pi for development
```

Needs only Docker and Python 3. Writes the pure wheel (`py3-none-any`: message classes, channel names, contract check), the native wheel, `contract-hash.txt` and `contract.json` to `builds/`. It also builds the native wheel for the Raspberry Pi. On an aarch64 machine (the CI's ARM runner) it is compiled in the `manylinux_2_28` container and carries a `manylinux` tag, so the same files go to the GitHub release and to PyPI. On an x86 machine it is cross compiled with the aarch64 compiler (nothing is emulated) and tried out under qemu-user; that wheel has the tag `linux_aarch64` and is only for `deploy.py` (`--mode cross` or `--mode native` forces a mode).

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

### Python (with the native module)

```bash
pip install ./python          # compiles the C++ transport
RACCOON_TRANSPORT_PURE=1 pip install ./python   # pure Python only
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

- **Plain publish/subscribe** — one file per channel, `/dev/shm/raccoon-rings/<channel with . for />` (`raccoon_ring`, a ring buffer with any number of writers and readers), no daemon. The geometry of a ring (slot count, slot size) comes from the channel table, and the first process that opens a channel creates the ring. A reader parks on a futex and is woken when a frame is published. A new reader starts after the latest frame; on a retained channel it receives the latest frame once. The ring file never shrinks, and a reader checks that the ring fits its file before it uses it.
- **Reliable delivery** (C++) — at-least-once for commands: the publisher re-sends until the subscriber acknowledges on `__raccoon/ack`, the subscriber delivers each command once. A command that is never acknowledged is reported through `set_reliable_event_handler` and counted in `TransportStats::reliable_dropped`.
- **Retain** (C++) — latest-value-on-subscribe, so a new subscriber immediately gets the most recent frame of the ring.
- **Lifecycle** — `shutdown()` waits for threads inside `spin_once()` before it closes the readers, and the destructor releases everything even after `stop()`. A handler that throws is counted (`TransportStats::handler_errors`), reported through `set_handler_error_handler` and does not stop the other handlers.
- **Frame size** — the receive buffer grows to the ring's payload size, so a subscriber never gets a cut-off frame.

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
- **Dart**: Message classes in `dart/lib/messages/` (written by hand, no code generation). `RaccoonRingTransport` is the C++ transport through `libraccoon_transport_bridge.so`, built with `cpp/bridge/build.sh`.
- **Python**: Message classes in `python/raccoon_transport/types/raccoon/`, channel names in `python/raccoon_transport/channel_table.py` (generated), and `raccoon_transport.Transport`, a pybind11 wrapper (`python/src/native.cpp`) around the C++ transport.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for how to add message types, run tests, and submit changes.

---

## License

Copyright (C) 2026 Tobias Madlberger
Licensed under the GNU General Public License v3.0 — see [COPYING](COPYING) for details.
