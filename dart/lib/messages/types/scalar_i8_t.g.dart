// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class ScalarI8T implements LcmMessage {

  int timestamp;
  int dir;

  ScalarI8T({
    required this.timestamp,
    required this.dir,
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
    buf.putInt8(dir);
  }

  static ScalarI8T decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static ScalarI8T decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final dir = buf.getInt8();

    return ScalarI8T(
      timestamp: timestamp,
      dir: dir,
    );
  }
}
