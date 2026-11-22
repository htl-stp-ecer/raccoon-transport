# Contributing to raccoon-transport

---

## Dev setup

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

There is no code generation step; the message classes are written by hand.

### Python

```bash
pip install ./python
```

---

## Adding a message type

Message types are written by hand in each language. C++ defines the wire layout; Python and Dart must encode and decode the same bytes. Every message starts with `int64_t timestamp` (microseconds since the epoch): the transport reads those first 8 bytes for latency statistics and as the correlation key of reliable delivery.

### 1. C++ (defines the layout)

- Header `messages/include/raccoon/my_value_t.hpp` with the struct, the constant `static constexpr const char* type_name = "my_value_t";` (the name that `channels.yml` uses) and the three members `encoded_size()`, `encode()` and `decode()`.
- Implementation `messages/types/my_value_t.cpp` using `codec::Writer` and `codec::Reader` from `raccoon/codec.hpp` (see `scalar_f_t.cpp`).
- Add the `.cpp` file to `messages/CMakeLists.txt`.

### 2. Python

Add `python/raccoon_transport/types/raccoon/my_value_t.py` (a dataclass with `encode()` and `decode()` built on `_codec`, see `scalar_f_t.py`) and import it in `python/raccoon_transport/types/raccoon/__init__.py`:

```python
from .my_value_t import my_value_t
```

### 3. Dart

Add `dart/lib/messages/types/my_value_t.g.dart` (the `.g.dart` suffix is only a name; the file is written by hand, see `scalar_f_t.g.dart`) and export it in `dart/lib/raccoon_transport.dart`.

### 4. Contract

```bash
python3 scripts/contract.py --write   # regenerate contract.json and the constants
python3 scripts/contract.py --check   # fails if Python or Dart differ from the C++ layout
```

Commit the regenerated files. A changed layout changes the contract hash, so all components have to be built against the new transport release.

---

## Adding a channel

A channel is an entry in `channels.yml`: its name, message type and the properties of its ring. The names in C++, Python and Dart are generated from it (see below), and the contract hash is computed from the table.

Channel names consist of `0-9 a-z A-Z _ -`, segments are separated by `/`. A name has the form `<root>/<device>/<kind>/<property>`, for example `hardware/motor/{port}/cmd/power`. A channel with a `cmd` segment is a command channel: commands are never deduplicated, because repeating the same command is meaningful (see `raccoon::dedup`).

After editing the table:

```bash
python3 scripts/channels.py --write   # names for C++, Python and Dart
python3 scripts/contract.py --write   # contract.json and the hash
```

Commit the regenerated files.

### The channel table

`channels.yml` lists every channel with its message type and the properties of its ring (`reliable`, `retained`, `slots`, `max_payload`). The schema is described in the comment at the top of the file. `scripts/channels.py` reads it without any package, checks it and writes `cpp/include/raccoon/ChannelTable.h`, `python/raccoon_transport/channel_table.py` and `dart/lib/src/channel_table.dart`. In every language a channel is named by its path: `hardware/motor/{port}/cmd/power` is `channels::hardware::motor::cmd::power(port)` in C++ and `channels.hardware.motor.cmd.power(port)` in Python and Dart, `hardware/imu/state/gyro` is `channels::hardware::imu::state::GYRO` in C++, `channels.hardware.imu.state.GYRO` in Python and `channels.hardware.imu.state.gyro` in Dart.

```bash
python3 scripts/channels.py --write   # regenerate the three generated files
python3 scripts/channels.py --check   # fails if the table is invalid or the header is out of date (also runs in CI)
python3 scripts/channels.py --list    # every channel with the size of its slots
```

The slot size of a message type with a fixed size follows from the type (`contract/contract.json`); a type without one needs a `max_payload` in the table.

---

## Running tests

### C++

```bash
ctest --test-dir build --output-on-failure
```

The suites cover the ring (`test_ring`), the transport lifecycle (`test_lifecycle_robust`), reliable delivery, the unit tests and the contract check.

The lifecycle and ring tests are meant to run under a sanitizer as well. Configure a separate build:

```bash
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_FLAGS="-fsanitize=thread" -DCMAKE_CXX_FLAGS="-fsanitize=thread" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

Use `-fsanitize=address` for AddressSanitizer.

### Contract

```bash
python3 scripts/contract.py --check
python3 -m unittest scripts/test_contract.py scripts/test_channels.py python/tests/test_contract_check.py
```

### Dart

```bash
cd dart
dart analyze
dart test test/contract_check_test.dart test/channel_table_test.dart
```

### Python

```bash
python3 -m pytest python/tests
```

### Integration tests (cross-language)

Python, C++ and Dart run as separate processes and talk through the shared-memory rings: every pair of languages for each message type, the encoded bytes, retained and reliable channels, and load.

```bash
# Prerequisites: build C++ (the interop tools are in build/cpp/tools) and install the Python package
pip install -e python/ pytest
PYTHONPATH=. pytest integration_tests/ -v -m "not dart"       # Python and C++

# The tests with Dart also need `dart` and the C bridge:
CXX=g++ CC_CROSS=gcc bash cpp/bridge/build.sh
PYTHONPATH=. pytest integration_tests/ -v -m dart
```

`RACCOON_BUILD_DIR` points at another CMake build directory, `RACCOON_BRIDGE_DIR` at another directory with `libraccoon_transport_bridge.so`. A test whose tool is missing is skipped. CI runs the tests without Dart as a gate and the Dart tests next to it.

---

## Code style

**C++**
- Namespace `raccoon`.
- PIMPL for the public API (`Transport`).
- `raccoon_ring.c` is C11 and exposes a C API; keep it free of C++.

**Python** targets Python 3.8+.

**Dart** follows standard conventions; run `dart analyze` before submitting.

---

## Pull request checklist

- [ ] Message types exist in C++, Python and Dart (if applicable)
- [ ] Channel constants match in C++, Python and Dart (if applicable)
- [ ] `python3 scripts/contract.py --write` was run and the result is committed
- [ ] `ctest --test-dir build --output-on-failure` passes
- [ ] Changes to the ring or `Transport` are covered by a test in `cpp/tests`
- [ ] `cd dart && dart analyze` is clean
