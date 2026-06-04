// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class ScalarFT implements LcmMessage {

  int timestamp;
  double value;

  ScalarFT({
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
    buf.putFloat32(value);
  }

  static ScalarFT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static ScalarFT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final value = buf.getFloat32();

    return ScalarFT(
      timestamp: timestamp,
      value: value,
    );
  }
}
