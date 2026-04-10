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
dart run build_runner build --delete-conflicting-outputs
```

### Python

```bash
pip install -e python/
```

---

## Adding a new LCM message type

This is the most common change. All three languages consume the same message definitions, but the pipelines differ.

### 1. Define the message

Create a `.lcm` file in `messages/types/`:

```
package raccoon;
struct my_value_t {
    int64_t timestamp;
    float   value;
}
```

### 2. C++ — automatic

The C++ build picks up new `.lcm` files from `messages/` automatically via CMake. No extra steps.

### 3. Python — regenerate

```bash
./scripts/generate-python-types.sh
```

This produces a new Python class in `python/raccoon_transport/types/raccoon/`. Then add the import to `python/raccoon_transport/types/raccoon/__init__.py`:

```python
from .my_value_t import my_value_t as my_value_t
```

### 4. Dart — copy and regenerate

Dart keeps its own local `.lcm` copies:

```bash
cp messages/types/my_value_t.lcm dart/lib/messages/
cd dart && dart run build_runner build --delete-conflicting-outputs
```

> **Why the copy?** The Dart package has its own LCM parser and code generator that runs via `build_runner`. It reads from `dart/lib/messages/`, not from the root `messages/` directory. This is the highest-risk drift point in the repo — always keep both in sync.

---

## Adding a channel

Channel constants are defined in three places (one per language). Keep them in sync:

| Language | File |
|:---------|:-----|
| C++ | `cpp/include/raccoon/Channels.hpp` |
| Python | `python/raccoon_transport/channels.py` |
| Dart | `dart/lib/src/channels.dart` |

All channels follow the naming pattern `raccoon/<device>/<property>`.

---

## Running tests

### Integration tests (cross-language)

```bash
# Prerequisites: build C++, Dart, and install Python package
PYTHONPATH=. pytest integration_tests/ -v

# Filter by language pair
PYTHONPATH=. pytest integration_tests/ -v -k "cpp"
PYTHONPATH=. pytest integration_tests/ -v -k "dart"

# Individual suites
PYTHONPATH=. pytest integration_tests/test_cross_pubsub.py -v
PYTHONPATH=. pytest integration_tests/test_message_compat.py -v
PYTHONPATH=. pytest integration_tests/test_retain.py -v
PYTHONPATH=. pytest integration_tests/test_stress.py -v
```

### Dart analysis

```bash
cd dart && dart analyze
```

### Python import check

```bash
python3 -c "from raccoon_transport import Transport, Channels; print('OK')"
```

---

## Code style

**C++** — follows existing conventions:
- Namespace: `raccoon`
- PIMPL pattern for public API (`Transport`)
- `LcmMessage` concept for type constraints

**Python** — targets Python 3.8+:
- Generated types are auto-formatted by the LCM generator — don't reformat them
- Hand-written code (transport, channels) should be clean and readable

**Dart** — standard Dart conventions:
- Run `dart analyze` before submitting
- Generated `.g.dart` files are not committed — they're rebuilt via `build_runner`

---

## Pull request checklist

- [ ] New `.lcm` file added to `messages/types/`
- [ ] Python types regenerated and `__init__.py` updated
- [ ] Dart `.lcm` copy added to `dart/lib/messages/`
- [ ] Channel constants added to all three languages (if applicable)
- [ ] Integration tests pass: `PYTHONPATH=. pytest integration_tests/ -v`
- [ ] Dart analysis clean: `cd dart && dart analyze`
