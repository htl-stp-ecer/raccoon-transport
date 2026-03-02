# raccoon-transport

Shared [LCM](https://lcm-proj.github.io/) transport library for robotics projects. Provides:

- All LCM message type definitions (`.lcm` files)
- Channel name constants in C++, Dart, and Python
- A transport layer wrapping LCM with optional **retain** (latest-value-on-subscribe) and **reliable** (at-least-once) delivery

## Repository Structure

```
messages/
  types/            .lcm message definitions (raccoon:: namespace)
  protocol/         .lcm protocol types for reliability/retain (raccoon:: namespace)
cpp/                C++ library (raccoon::Transport, raccoon::Channels)
  tools/            C++ interop helpers for integration tests
dart/               Dart package with built-in LCM parser + code generator
python/             Python package with pre-generated types
integration_tests/  Cross-language integration + stress tests
scripts/            Helper scripts (e.g. Python type generation)
```

## Integration

### C++ (CMake with FetchContent)

In your `CMakeLists.txt`:

```cmake
include(FetchContent)

FetchContent_Declare(
    raccoon_transport
    GIT_REPOSITORY git@github.com:htl-stp-ecer/raccoon-transport.git
    GIT_TAG main
)
FetchContent_MakeAvailable(raccoon_transport)

target_link_libraries(my_target PRIVATE raccoon::transport)
```

Usage:

```cpp
#include <raccoon/Transport.h>
#include <raccoon/Channels.h>
#include <raccoon/scalar_f_t.hpp>

auto transport = raccoon::Transport::create();

// Publish
raccoon::scalar_f_t msg{};
msg.timestamp = 0;
msg.value = 42.0f;
transport.publish(raccoon::Channels::GYRO, msg);

// Publish with retain (new subscribers get the latest value)
transport.publish(raccoon::Channels::GYRO, msg, {.retained = true});

// Publish with reliable delivery (at-least-once, retransmits until ACKed)
transport.publish(raccoon::Channels::SHUTDOWN_CMD, msg, {.reliable = true});

// Reliable with custom retry settings
transport.publish(raccoon::Channels::SHUTDOWN_CMD, msg,
    {.reliable = true, .retryInterval = std::chrono::milliseconds(200), .maxRetries = 5});

// Subscribe
transport.subscribe<raccoon::scalar_f_t>(raccoon::Channels::GYRO, [](const raccoon::scalar_f_t& msg) {
    // handle message
});

// Subscribe and request the retained (latest) value
transport.subscribe<raccoon::scalar_f_t>(raccoon::Channels::GYRO, handler,
    {.requestRetained = true});

// Subscribe with reliable delivery (receives envelopes, sends ACKs, deduplicates)
transport.subscribe<raccoon::scalar_f_t>(raccoon::Channels::SHUTDOWN_CMD, handler,
    {.reliable = true});

// Parametric channels (port-indexed)
transport.publish(raccoon::Channels::motorPower(0), msg);

// Event loop
transport.spin();           // blocks forever
transport.spinOnce(100);    // handle one message, 100ms timeout
```

### Dart

In your `pubspec.yaml`:

```yaml
dependencies:
  raccoon_transport:
    git:
      url: git@github.com:htl-stp-ecer/raccoon-transport.git
      path: dart
```

Usage:

```dart
import 'package:raccoon_transport/raccoon_transport.dart';

final transport = await RaccoonTransport.create();

// Publish a typed message
final msg = ScalarFT()
  ..timestamp = 0
  ..value = 42.0;
transport.publishMessage(Channels.gyro, msg);

// Publish with retain
transport.publishMessage(Channels.gyro, msg,
    options: PublishOptions(retained: true));

// Publish with reliable delivery
transport.publishMessage(Channels.shutdownCmd, msg,
    options: PublishOptions(reliable: true));

// Reliable with custom retry settings
transport.publishMessage(Channels.shutdownCmd, msg,
    options: PublishOptions(
      reliable: true,
      retryInterval: Duration(milliseconds: 200),
      maxRetries: 5,
    ));

// Subscribe
transport.subscribe(Channels.gyro, (channel, data) {
  // handle raw message bytes
});

// Subscribe and request the retained (latest) value
transport.subscribe(Channels.gyro, handler,
    options: SubscribeOptions(requestRetained: true));

// Subscribe with reliable delivery
transport.subscribe(Channels.shutdownCmd, handler,
    options: SubscribeOptions(reliable: true));

// Parametric channels
transport.publishMessage(Channels.motorPower(0), msg);

// Clean up
transport.dispose();
```

The Dart package includes a built-in LCM parser and code generator. Generated `.g.dart` files are produced via `build_runner`:

```bash
cd dart
dart pub get
dart run build_runner build
```

### Python

Install as an editable package:

```bash
pip install -e "git+ssh://git@github.com/htl-stp-ecer/raccoon-transport.git#subdirectory=python"
```

Or for local development:

```bash
pip install -e ../raccoon-transport/python
```

Usage:

```python
from raccoon_transport import Transport, Channels
from raccoon_transport.types.exlcm import scalar_f_t

transport = Transport.create()

# Publish
msg = scalar_f_t()
msg.timestamp = 0
msg.value = 42.0
transport.publish(Channels.GYRO, msg)

# Publish with retain
transport.publish(Channels.GYRO, msg, retained=True)

# Publish with reliable delivery
transport.publish(Channels.SHUTDOWN_CMD, msg, reliable=True)

# Reliable with custom retry settings
transport.publish(Channels.SHUTDOWN_CMD, msg,
                  reliable=True, retry_interval_ms=200, max_retries=5)

# Subscribe
def handler(channel, data):
    msg = scalar_f_t.decode(data)
    print(msg.value)

transport.subscribe(Channels.GYRO, handler)

# Subscribe and request the retained (latest) value
transport.subscribe(Channels.GYRO, handler, request_retained=True)

# Subscribe with reliable delivery
transport.subscribe(Channels.SHUTDOWN_CMD, handler, reliable=True)

# Parametric channels
transport.publish(Channels.motor_power(0), msg)

# Event loop
transport.spin()            # blocks forever
transport.spin_once(100)    # handle one message, 100ms timeout
```

## Reliable Delivery (At-Least-Once)

The transport supports an optional **reliable** mode that guarantees at-least-once delivery for critical messages (e.g. motor commands, shutdown signals). When enabled:

1. **Publisher** wraps the payload in an `envelope_t` (with a unique publisher ID and sequence number), publishes it on `__raccoon/r/{channel}`, and queues it for retransmission
2. **Subscriber** listens on `__raccoon/r/{channel}`, decodes the envelope, sends an `ack_t` back on `__raccoon/ack`, deduplicates by `(publisher_id, seq_num)`, and delivers the inner payload to the user handler
3. **Retransmission** happens automatically on each `spinOnce()`/`spin()` tick. Pending messages past `retryInterval` are resent. After `maxRetries` attempts without an ACK, the message is dropped with a warning
4. **Deduplication** uses a ring buffer of the last 1000 `(publisher_id, seq_num)` pairs, so retransmitted envelopes are only delivered once

### When to use reliable mode

| Use case | Mode |
|---|---|
| High-frequency sensor data (gyro, accelerometer) | Plain (default) |
| Motor commands, servo positions | **Reliable** |
| Shutdown commands, mode switches | **Reliable** |
| Screen render requests | Plain or reliable depending on criticality |

### Configuration

| Parameter | Default | Description |
|---|---|---|
| `retryInterval` | 100 ms | Time between retransmission attempts |
| `maxRetries` | 10 | Maximum retransmission attempts before dropping |

Both publisher and subscriber must use `reliable = true` for the protocol to work. A reliable publisher with a plain subscriber (or vice versa) will not deliver messages, since they use different channel paths.

### Reliable + Retain

The two features compose naturally. Publishing with both `reliable = true` and `retained = true` ensures that the message is delivered at least once to active subscribers **and** cached for future subscribers who request the retained value.

## Channel Naming Convention

All channels follow the pattern `libstp/<device>/<property>`. Port-indexed devices include the port number: `libstp/<device>/<port>/<property>`.

Internal protocol channels use the `__raccoon/` prefix and should not be used directly.

## Message Types

LCM message definitions live in `messages/types/` under the `raccoon` package:

| Type | Description |
|---|---|
| `scalar_f_t` | Single float value |
| `scalar_i32_t` | Single int32 value |
| `scalar_i8_t` | Single int8 value |
| `string_t` | String value |
| `vector3f_t` | 3D float vector |
| `quaternion_t` | Quaternion (w, x, y, z) |
| `orientation_matrix_t` | 3x3 orientation matrix |
| `yolo_box_t` | Single YOLO detection bounding box |
| `yolo_frame_t` | Frame of YOLO detections |
| `screen_render_t` | Screen render command |
| `screen_render_answer_t` | Screen render response |

All message types include a `int64_t timestamp` field.

### Protocol Types

Internal types in `messages/protocol/` used by the transport layer (not for direct use):

| Type | Description |
|---|---|
| `envelope_t` | Wraps a payload for reliable delivery (publisher ID, sequence number, inner payload) |
| `ack_t` | Acknowledgement sent by reliable subscribers back to the publisher |
| `retain_request_t` | Request to replay a cached retained value |

## Building from Source

### C++

```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j$(nproc)
```

Requires CMake >= 3.16 and a C++20 compiler. LCM is fetched automatically via FetchContent.

### Dart

```bash
cd dart
dart pub get
dart run build_runner build
```

### Python

```bash
pip install -e python/
```

To regenerate Python types from `.lcm` definitions (requires `lcm-gen`):

```bash
./scripts/generate-python-types.sh
```

## Running Tests

The integration test suite verifies cross-language compatibility across all three implementations (C++, Dart, Python). Tests cover pub/sub, retain protocol, wire-format encoding, and stress scenarios.

### Prerequisites

Build/install all three languages first:

```bash
# C++ (builds the library and interop test helpers)
mkdir -p build && cd build
cmake ..
cmake --build . -j$(nproc)
cd ..

# Dart
cd dart
dart pub get
dart run build_runner build
cd ..

# Python
pip install -e python/
```

### Running the full suite

```bash
PYTHONPATH=. pytest integration_tests/ -v
```

### Running specific test files

```bash
# Cross-language pub/sub (Python <-> Dart <-> C++)
PYTHONPATH=. pytest integration_tests/test_cross_pubsub.py -v

# Wire-format compatibility (fingerprints, encode/decode round-trips)
PYTHONPATH=. pytest integration_tests/test_message_compat.py -v

# Retain protocol (publish with retain, subscribe with request)
PYTHONPATH=. pytest integration_tests/test_retain.py -v

# Stress & concurrency (high-throughput, multi-channel, tri-language)
PYTHONPATH=. pytest integration_tests/test_stress.py -v

# Reliable delivery (at-least-once, dedup, max retries)
PYTHONPATH=. pytest integration_tests/test_reliable.py -v
```

### Filtering by language

```bash
# Only C++ cross-language tests
PYTHONPATH=. pytest integration_tests/ -v -k "cpp"

# Only Dart cross-language tests
PYTHONPATH=. pytest integration_tests/ -v -k "dart"
```

### Test matrix

| Suite | Tests | What it covers |
|---|---|---|
| `test_cross_pubsub` | 10 | All 6 directional pairs (Py/Dart/C++ publish and subscribe) |
| `test_message_compat` | 6 | Fingerprint match + hex encode/decode across all 3 languages |
| `test_retain` | 11 | Retain protocol within and across all language combinations |
| `test_reliable` | 4 | Reliable delivery, deduplication, max retries, multi-message |
| `test_stress` | 5 | Tri-language simultaneous pub/sub, 100-msg throughput, multi-channel |

## Contributing

### Adding a New Message Type

1. Create a `.lcm` file in `messages/types/` (use the `raccoon` package name)
2. Rebuild the C++ project -- CMake generates the C++ header automatically
3. Run `dart run build_runner build` in `dart/` to generate the Dart type
4. Run `./scripts/generate-python-types.sh` to generate the Python type
5. Export the new Dart type in `dart/lib/raccoon_transport.dart`

### Adding a New Channel

Add the channel constant in all three languages to keep them in sync:

- `cpp/include/raccoon/Channels.h`
- `dart/lib/src/channels.dart`
- `python/raccoon_transport/channels.py`
