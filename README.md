# raccoon-transport

`raccoon-transport` is the message and transport layer shared across the robotics stack. It combines four concerns in one repo:

- the authoritative C++-side `.lcm` message definitions under `messages/`
- a C++ transport wrapper around LCM with retain and reliable-delivery support
- a thin Python transport package plus generated Python message types
- a Dart package that includes its own LCM runtime, parser, generator, and generated message set

This package is best treated as a small monorepo rather than a single library.

## Repository Layout

```text
raccoon-transport/
├── messages/            # Source .lcm files used by the C++ build
├── cpp/                 # C++ transport library and interop tools
├── python/              # Python package and pre-generated message classes
├── dart/                # Dart transport package, parser, generator, generated messages
├── integration_tests/   # Cross-language compatibility and stress tests
└── scripts/             # Helper scripts, including Python type generation
```

## Architecture

### Message Schema Layer

The source-of-truth message files used by the C++ build live under:

- `messages/types/`
- `messages/protocol/`

Protocol messages:

- `envelope_t`
- `ack_t`
- `retain_request_t`

Those protocol types implement reliable delivery and retain replay across languages.

### C++ Layer

The main C++ API is:

- `raccoon::Transport`
- `raccoon::PublishOptions`
- `raccoon::SubscribeOptions`
- `raccoon::Channels`

Important details:

- Reliable delivery is envelope + ACK based.
- Retained delivery replays the last cached payload for a channel.
- Deduplication and transport latency statistics are public C++ features.
- Reliable retransmission advances during `spinOnce()` and `spin()`.

### Python Layer

The Python package is intentionally thinner than the C++ one:

- `raccoon_transport.Transport`
- `raccoon_transport.Channels`
- `raccoon_transport.ProtocolChannels`
- generated message classes under `raccoon_transport.types.raccoon`

Python implements reliable and retained behavior in pure Python on top of `lcm.LCM`.

### Dart Layer

The Dart package exports more than just transport:

- `RaccoonTransport`
- channel constants
- the raw `Lcm` runtime
- parser and generator internals
- generated message classes

Unlike the C++ and Python side, Dart keeps a duplicated `.lcm` tree under `dart/lib/messages/` and generates code from that local copy.

## Public APIs

### C++

Headers:

- `cpp/include/raccoon/Transport.h`
- `cpp/include/raccoon/Options.h`
- `cpp/include/raccoon/Channels.h`

### Python

Package entrypoints:

- `raccoon_transport.Transport`
- `raccoon_transport.Channels`
- `raccoon_transport.ProtocolChannels`

Generated types live under:

- `raccoon_transport.types.raccoon`

### Dart

Main library:

- `dart/lib/raccoon_transport.dart`

That export surface includes transport, channels, raw LCM support, parser/generator code, and generated message classes.

## Reliability And Retain

Reliable delivery is opt-in and only works when both ends agree to use it.

Rules contributors must preserve:

- reliable publishing wraps payloads in `envelope_t`
- subscribers ACK with `ack_t`
- deduplication keys are based on publisher id and sequence number
- retained replay uses `retain_request_t`

One subtle but important implementation detail: `retain_request_t` handling is partially manual in every language. The C++, Python, and Dart implementations do not all rely on generated decoders in the same way. Any change to that message or its on-wire interpretation must be validated across all three languages.

## Build And Test

### C++

The root CMake build:

- fetches `lcm`
- generates message code from `messages/`
- builds the C++ transport library
- builds the C++ interop tools used by integration tests

### Python

Local development:

```bash
pip install -e raccoon-transport/python
```

### Dart

```bash
cd raccoon-transport/dart
dart pub get
dart run build_runner build
```

### Integration Tests

High-value tests live in `integration_tests/`:

- cross-language pub/sub
- message compatibility
- retain replay
- reliable delivery
- stress tests

These are the best safety net for protocol-level changes.

## Contributor Notes

### Adding A Message Type

When you add or change a message, check all of these:

1. Update the source `.lcm` file under `messages/`.
2. Regenerate or update the Python message classes if needed.
3. Update the duplicated Dart `.lcm` files under `dart/lib/messages/` and rerun `build_runner`.
4. Verify channel usage and any helper constants.
5. Run the compatibility and retain / reliable integration tests.

### Avoiding Common Drift

The highest-risk documentation mismatch in this repo has been assuming all languages share the exact same generated source pipeline. They do not. C++ is driven by `messages/`, but Dart keeps its own local `.lcm` copies. Any contributor README or automation change should make that split explicit.
