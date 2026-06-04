// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class YoloBoxT implements LcmMessage {

  int timestamp;
  double x;
  double y;
  double width;
  double height;
  String label;
  double confidence;

  YoloBoxT({
    required this.timestamp,
    required this.x,
    required this.y,
    required this.width,
    required this.height,
    required this.label,
    required this.confidence,
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
    buf.putFloat32(width);
    buf.putFloat32(height);
    {
      final bytes = utf8.encode(label);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
    buf.putFloat32(confidence);
  }

  static YoloBoxT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static YoloBoxT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final x = buf.getFloat32();
    final y = buf.getFloat32();
    final width = buf.getFloat32();
    final height = buf.getFloat32();
    final label = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();
    final confidence = buf.getFloat32();

    return YoloBoxT(
      timestamp: timestamp,
      x: x,
      y: y,
      width: width,
      height: height,
      label: label,
      confidence: confidence,
    );
  }
}
