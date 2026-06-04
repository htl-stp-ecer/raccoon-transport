// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class OrientationMatrixT implements LcmMessage {

  int timestamp;
  List<int> m;

  OrientationMatrixT({
    required this.timestamp,
    required this.m,
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
    for (var i0 = 0; i0 < 9; i0++) {
      buf.putInt8(m[i0]);
    }
  }

  static OrientationMatrixT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static OrientationMatrixT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final m = <int>[];
    for (var i0 = 0; i0 < 9; i0++) {
      final mElement = buf.getInt8();
      m.add(mElement);
    }

    return OrientationMatrixT(
      timestamp: timestamp,
      m: m,
    );
  }
}
