# raccoon-transport

Shared [LCM](https://lcm-proj.github.io/) transport library for robotics projects. Provides:

- All LCM message type definitions (`.lcm` files)
- Channel name constants in C++, Dart, and Python
- A transport layer wrapping LCM with optional **retain** (latest-value-on-subscribe) and **reliable** (at-least-once) delivery

## Repository Structure

```
messages/
  types/          .lcm message definitions (raccoon:: namespace)
  protocol/       .lcm protocol types for reliability/retain (raccoon:: namespace)
cpp/              C++ library (raccoon::Transport, raccoon::Channels)
dart/             Dart package with built-in LCM parser + code generator
python/           Python package with pre-generated types
scripts/          Helper scripts (e.g. Python type generation)
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
#include <exlcm/scalar_f_t.hpp>

auto transport = raccoon::Transport::create();

// Publish
exlcm::scalar_f_t msg{};
msg.timestamp = 0;
msg.value = 42.0f;
transport.publish(raccoon::Channels::GYRO, msg);

// Publish with retain (new subscribers get the latest value)
transport.publish(raccoon::Channels::GYRO, msg, {.retained = true});

// Subscribe
transport.subscribe<exlcm::scalar_f_t>(raccoon::Channels::GYRO, [](const exlcm::scalar_f_t& msg) {
    // handle message
});

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

// Subscribe
transport.subscribe(Channels.gyro, (channel, data) {
  // handle raw message bytes
});

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

# Subscribe
def handler(channel, data):
    msg = scalar_f_t.decode(data)
    print(msg.value)

transport.subscribe(Channels.GYRO, handler)

# Parametric channels
transport.publish(Channels.motor_power(0), msg)

# Event loop
transport.spin()            # blocks forever
transport.spin_once(100)    # handle one message, 100ms timeout
```

## Channel Naming Convention

All channels follow the pattern `libstp/<device>/<property>`. Port-indexed devices include the port number: `libstp/<device>/<port>/<property>`.

Internal protocol channels use the `__raccoon/` prefix and should not be used directly.

## Message Types

LCM message definitions live in `messages/types/` under the `exlcm` package:

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

## Contributing

### Adding a New Message Type

1. Create a `.lcm` file in `messages/types/` (use the `exlcm` package name)
2. Rebuild the C++ project -- CMake generates the C++ header automatically
3. Run `dart run build_runner build` in `dart/` to generate the Dart type
4. Run `./scripts/generate-python-types.sh` to generate the Python type
5. Export the new Dart type in `dart/lib/raccoon_transport.dart`

### Adding a New Channel

Add the channel constant in all three languages to keep them in sync:

- `cpp/include/raccoon/Channels.h`
- `dart/lib/src/channels.dart`
- `python/raccoon_transport/channels.py`
