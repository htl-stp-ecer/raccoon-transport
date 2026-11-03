// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class KinematicsConfigT implements LcmMessage {

  int timestamp;
  List<double> inv_matrix;
  List<double> ticks_to_rad;
  List<double> fwd_matrix;
  List<double> bemf_offset;

  KinematicsConfigT({
    required this.timestamp,
    required this.inv_matrix,
    required this.ticks_to_rad,
    required this.fwd_matrix,
    // Added to the C++ wire format after the first three arrays; optional here
    // so existing callers keep compiling.
    this.bemf_offset = const [0.0, 0.0, 0.0, 0.0],
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
    for (var i0 = 0; i0 < 12; i0++) {
      buf.putFloat32(inv_matrix[i0]);
    }
    for (var i0 = 0; i0 < 4; i0++) {
      buf.putFloat32(ticks_to_rad[i0]);
    }
    for (var i0 = 0; i0 < 12; i0++) {
      buf.putFloat32(fwd_matrix[i0]);
    }
    for (var i0 = 0; i0 < 4; i0++) {
      buf.putFloat32(bemf_offset[i0]);
    }
  }

  static KinematicsConfigT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static KinematicsConfigT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final inv_matrix = <double>[];
    for (var i0 = 0; i0 < 12; i0++) {
      final inv_matrixElement = buf.getFloat32();
      inv_matrix.add(inv_matrixElement);
    }
    final ticks_to_rad = <double>[];
    for (var i0 = 0; i0 < 4; i0++) {
      final ticks_to_radElement = buf.getFloat32();
      ticks_to_rad.add(ticks_to_radElement);
    }
    final fwd_matrix = <double>[];
    for (var i0 = 0; i0 < 12; i0++) {
      final fwd_matrixElement = buf.getFloat32();
      fwd_matrix.add(fwd_matrixElement);
    }
    final bemf_offset = <double>[];
    for (var i0 = 0; i0 < 4; i0++) {
      final bemf_offsetElement = buf.getFloat32();
      bemf_offset.add(bemf_offsetElement);
    }

    return KinematicsConfigT(
      timestamp: timestamp,
      inv_matrix: inv_matrix,
      ticks_to_rad: ticks_to_rad,
      fwd_matrix: fwd_matrix,
      bemf_offset: bemf_offset,
    );
  }
}
