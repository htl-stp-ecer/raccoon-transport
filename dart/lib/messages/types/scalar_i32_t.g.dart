// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class ScalarI32T implements LcmMessage {

  int timestamp;
  int value;

  ScalarI32T({
    required this.timestamp,
    required this.value,
  });

  @override
  int get lcmFingerprint => 0;

  @override
  void encode(LcmBuffer buf) {
    encodeBody(buf);
  }

  @override
  void encodeBody(LcmBuffer buf) {
    buf.putInt64(timestamp);
    buf.putInt32(value);
  }

  static ScalarI32T decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static ScalarI32T decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final value = buf.getInt32();

    return ScalarI32T(
      timestamp: timestamp,
      value: value,
    );
  }
}
