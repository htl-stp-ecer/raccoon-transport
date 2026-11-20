# CLAUDE.md

## Project Overview

raccoon-transport is a shared LCM (Lightweight Communications and Marshalling) transport library used across three robotics projects. It provides:
1. All LCM message type definitions (replacing the `lcm-messages` git submodule)
2. Channel definitions in C++, Dart, and Python
3. A transport layer with optional at-least-once reliable delivery and retain (latest-value-on-subscribe)

## Contract hash

Components must agree on the wire layout, so the transport has one short
identifier of it: the contract hash. It is computed by `scripts/contract.py`
from the C++ sources (message structs, channel names, ring version) and written
to `contract/contract.json` plus generated constants for C++
(`cpp/include/raccoon/Contract.h`), Python (`raccoon_transport/_contract.py`)
and Dart (`dart/lib/src/contract.dart`). Every binary also carries the string
`RACCOON_CONTRACT_V1=<hash>` so a build can read it without running it.

- After changing a message struct, a channel or the ring layout, run
  `python3 scripts/contract.py --write` and commit the result.
- `python3 scripts/contract.py --check` (also run in CI) fails if a generated
  file is stale or if the Python/Dart messages drifted from C++.
- Tests: `python3 -m unittest scripts/test_contract.py`.
- The release publishes `contract-hash.txt` and `contract.json` as assets.
- Startup check: `contract/CHECK_SPEC.md` is the one description of the check
  (decision table, messages, status files, exit code 78); `contract/check_vectors.json`
  holds the test cases that the C++ (`ContractCheck.h`), Python
  (`raccoon_transport/contract_check.py`) and Dart (`contract_check.dart`)
  implementations all pass. Change the spec and the vectors first, then all three.
- C++ parses the bundle file with the vendored nlohmann/json (MIT) in
  `cpp/third_party/nlohmann`; keep `LICENSE.MIT` and `THIRD_PARTY_NOTICES.md`.
- The wire format is hand-written in each language; the `.lcm` / LCM sections
  below are historical.

## Build Commands

### Wheel and contract assets (Docker)
```bash
python3 build.py                  # contract check + wheel + contract assets -> builds/, version 0.0
python3 build.py --version 71     # release build (what CI does)
python3 deploy.py --host <pi>     # install builds/ wheel on the Pi, sets raccoon-transport to "dev" in its bundle file
```
Only `build.py` and `deploy.py` build and install the wheel; CI runs `build.py` too. `deploy.py` is for local builds; releases are installed by `raccoon update`. C++ and Dart are used as source (submodule) and tested in CI. Do not remove `cpp/bridge/build.sh`: botui and stm32-data-reader build their ring bridge with it.

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

### Integration Tests:
```bash
# Prerequisites: build C++, Dart, and install Python package (see above)
PYTHONPATH=. pytest integration_tests/ -v

# Run a specific suite
PYTHONPATH=. pytest integration_tests/test_cross_pubsub.py -v
PYTHONPATH=. pytest integration_tests/test_message_compat.py -v
PYTHONPATH=. pytest integration_tests/test_retain.py -v
PYTHONPATH=. pytest integration_tests/test_stress.py -v

# Filter by language
PYTHONPATH=. pytest integration_tests/ -v -k "cpp"
PYTHONPATH=. pytest integration_tests/ -v -k "dart"
```

## Architecture

### Message Types
- `messages/types/` — 11 `raccoon::` message types
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

### C++ Interop Tools (`cpp/tools/`)
- `interop_publish` / `interop_subscribe` — CLI tools with JSON event protocol
- Used by integration tests to test C++ in cross-language scenarios
- Built automatically with the C++ library

### Integration Tests (`integration_tests/`)
- 32 tests covering all 6 directional language pairs (Py/Dart/C++)
- `helpers/dart_runner.py` — DartHelper subprocess manager
- `helpers/cpp_runner.py` — CppHelper subprocess manager
- Tests: cross-pubsub, message compat, retain protocol, stress/concurrency

### Channel Naming
All channels follow pattern `raccoon/<device>/<property>`.
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
