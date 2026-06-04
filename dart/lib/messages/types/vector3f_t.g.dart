// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class Vector3fT implements LcmMessage {

  int timestamp;
  double x;
  double y;
  double z;

  Vector3fT({
    required this.timestamp,
    required this.x,
    required this.y,
    required this.z,
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
    buf.putFloat32(x);
    buf.putFloat32(y);
    buf.putFloat32(z);
  }

  static Vector3fT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static Vector3fT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final x = buf.getFloat32();
    final y = buf.getFloat32();
    final z = buf.getFloat32();

    return Vector3fT(
      timestamp: timestamp,
      x: x,
      y: y,
      z: z,
    );
  }
}
