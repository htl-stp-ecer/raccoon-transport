// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class QuaternionT implements LcmMessage {

  int timestamp;
  double w;
  double x;
  double y;
  double z;

  QuaternionT({
    required this.timestamp,
    required this.w,
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
    buf.putFloat32(w);
    buf.putFloat32(x);
    buf.putFloat32(y);
    buf.putFloat32(z);
  }

  static QuaternionT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static QuaternionT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final w = buf.getFloat32();
    final x = buf.getFloat32();
    final y = buf.getFloat32();
    final z = buf.getFloat32();

    return QuaternionT(
      timestamp: timestamp,
      w: w,
      x: x,
      y: y,
      z: z,
    );
  }
}
