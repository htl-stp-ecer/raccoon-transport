# CLAUDE.md

## Project Overview

raccoon-transport is the messaging layer shared by the Raccoon components (raccoon-lib, stm32-data-reader, botui, raccoon-cli, raccoon-cam). It provides:
1. The message types, written by hand in C++, Python and Dart (C++ defines the wire layout).
2. The channel names in C++, Python and Dart.
3. A shared-memory transport: one ring buffer file per channel under `/dev/shm`, no daemon.
4. In the C++ transport: at-least-once delivery for commands (ACK and retransmit) and latest-value-on-subscribe (retain).
5. A contract hash that lets every component check at startup that it speaks the same wire format.

## How the transport works

- Each channel is a file `/dev/shm/raccoon-rings/<name>` (the directory has mode 0777, the files 0666; a `/` in the channel name is written as `.`). The geometry of a ring (slot count, slot stride, maximum payload) comes from the channel table. The first process that opens the channel, reader or writer, creates the file under an `flock` and writes the header (magic, version, slot count, slot size, flags, channel name); every later process attaches to it. There is no placeholder file.
- Every ring accepts several writers. A writer takes a slot ticket with an atomic `fetch_add`, fills the slot and advances `producer_seq` to the highest finished ticket; a reader waits a moment for a claimed slot to be filled and skips it after 2 ms if the writer died.
- A slot occupies `round64(16 + max_payload)` bytes: a 16-byte slot header (sequence number, length) and the payload, aligned to a cache line.
- A new reader starts after the latest frame. A reader of a retained channel receives the latest frame once, then the frames that follow.
- The writer fills a slot, marks it with a sequence number and wakes parked readers through a futex word in the header. A reader copies a slot and checks the sequence number again afterwards (seqlock), so a slot that was overwritten during the copy is detected. A reader that falls a whole ring behind skips to the oldest intact slot. A reader checks about once a second, while it is idle, that its ring file is still the one on disk (`rrb_reader_revalidate`).
- The ring file never shrinks: readers may have the larger file mapped, and a page beyond a shortened end of file would raise SIGBUS in them. A reader checks that the header geometry fits the file before it uses a ring.
- `rrb_reader_recv` reports a frame that does not fit the buffer as `RRB_RECV_TOO_BIG` and keeps it; `rrb_reader_skip` drops it. `raccoon::Transport` grows its receive buffer to the ring's payload size, so subscribers never see a cut-off frame.
- `raccoon::Transport::shutdown()` wakes threads inside `spin_once()` and waits (up to one second) for them before it closes the readers. `stop()` only ends `spin()`; the destructor releases everything.
- A handler that throws does not stop the spin loop. The error is counted (`TransportStats::handler_errors`), the first few per channel are printed to stderr, and `set_handler_error_handler` receives each one.
- Use one thread per `Transport` for `spin_once()`.
- `publish_raw`, `publish_typed` and `publish<T>` return a `PublishResult`: `Ok`, `NotInTable` (the channel is not in `channels.yml`), `TypeMismatch` (wrong message type, or a size other than the one of a fixed-size type), `TooBig` (above the limit of the channel) or `Closed` (the transport was shut down). `subscribe_raw` and `subscribe<T>` return `false` for the same reasons. Names follow the Rust convention: types and enum values in `PascalCase`, functions and variables in `snake_case`.

## Contract hash

Components must agree on the wire layout, so the transport has one short
identifier of it: the contract hash. It is computed by `scripts/contract.py`
from the C++ message structs, `channels.yml` and the ring version, and written
to `contract/contract.json` plus generated constants for C++
(`cpp/include/raccoon/Contract.h`), Python (`raccoon_transport/_contract.py`)
and Dart (`dart/lib/src/contract.dart`). Every binary also carries the string
`RACCOON_CONTRACT_V1=<hash>` so a build can read it without running it.

- After changing a message struct, a channel or the ring layout, run
  `python3 scripts/contract.py --write` and commit the result.
- `python3 scripts/contract.py --check` (also run in CI) fails if a generated
  file is stale or if the Python/Dart messages drifted from C++.
- Tests: `python3 -m unittest scripts/test_contract.py scripts/test_channels.py`.
- The release publishes `contract-hash.txt` and `contract.json` as assets.
- Startup check: the decision table is implemented identically in C++
  (`cpp/src/ContractCheck.cpp`), Python (`raccoon_transport/contract_check.py`) and
  Dart (`dart/lib/src/contract_check.dart`); the status files and exit code 78 are
  defined there. `contract/check_vectors.json` holds the test cases that all three
  implementations pass. Change the vectors first, then all three. The component
  version check is described in `contract/COMPONENT_VERSION_CHECK.md`.
- C++ parses the bundle file with the vendored nlohmann/json (MIT) in
  `cpp/third_party/nlohmann`; keep `LICENSE.MIT` and `THIRD_PARTY_NOTICES.md`.

## Channel table

`channels.yml` is the list of every channel with its message type and the properties of its ring. `scripts/channels.py --write` generates `cpp/include/raccoon/ChannelTable.h`, `python/raccoon_transport/channel_table.py` and `dart/lib/src/channel_table.dart` from it (`--check` runs in CI and in `build.py`). A channel is named by its path in every language, for example `channels::hardware::motor::cmd::power(port)` in C++ and `channels.hardware.motor.cmd.power(port)` in Python and Dart. The slot size of a fixed-size message type follows from the type, other types carry a `max_payload` in the table.

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
ctest --output-on-failure
```

### Dart:
```bash
cd dart
dart pub get
dart analyze
```

### Python:
```bash
cd python
pip install -e .
```

### Integration Tests:
```bash
# Prerequisites: build C++, Dart, and install Python package (see above)
PYTHONPATH=. pytest integration_tests/ -v
PYTHONPATH=. pytest integration_tests/ -v -k "cpp"
PYTHONPATH=. pytest integration_tests/ -v -k "dart"
```
CI runs them without gating the release.

## Architecture

### Message Types
- `messages/include/raccoon/*.hpp` and `messages/types/*.cpp` — the `raccoon::` message structs with hand-written `encode`/`decode` (big-endian, no fingerprint). The first field of every message is `int64_t timestamp`.
- `messages/protocol/` — the protocol types for reliable delivery and retain.
- `python/raccoon_transport/types/raccoon/` and `dart/lib/messages/types/*.g.dart` — the same layouts in Python and Dart (`contract.py --check` verifies them against C++).
- Adding a type is described in `CONTRIBUTING.md`.

### C++ Library (`cpp/`)
- `raccoon_ring.c` / `raccoon_ring.h` — the ring buffer (C API): writer, reader, futex wake, multi-channel wait.
- `raccoon::Transport` — the public API (PIMPL): publish, subscribe, `spin_once`, reliable delivery, retain, statistics.
- `ChannelTable.h` (generated) — every channel with its message type and ring properties; `raccoon::dedup::isCommandChannel` decides which channels are never deduplicated (a name with a `cmd` segment).
- `cpp/bridge/` — a C library (`libraccoon_ring_bridge.so`) that the Dart package loads through FFI.

### Dart Package (`dart/`)
- `RaccoonRingTransport` (`lib/src/ring/transport.dart`) reaches the rings through the C bridge.
- `lib/src/lcm/`, `lib/src/transport.dart` and `lib/src/reliable.dart` are a separate UDP-multicast client that does not use shared memory.

### Python Package (`python/`)
- Message classes in `raccoon_transport/types/raccoon/` and the channel names in `channels.py`.
- `raccoon_transport.Transport` is an in-process queue. It does not use shared memory and does not reach other processes. To talk to the robot from Python, use `raccoon.foundation.get_transport()` of raccoon-lib, which binds the C++ transport.

### C++ Interop Tools (`cpp/tools/`)
- `interop_publish` / `interop_subscribe` — CLI tools with a JSON event protocol, used by the integration tests.

### Channel Naming
Channels follow the pattern `raccoon/<device>/<property>`.
Protocol channels use the `__raccoon/` prefix.

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
