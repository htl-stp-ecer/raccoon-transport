// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class CamBlobT implements LcmMessage {

  int timestamp;
  String label;
  double x;
  double y;
  double width;
  double height;
  int area;
  double confidence;

  CamBlobT({
    required this.timestamp,
    required this.label,
    required this.x,
    required this.y,
    required this.width,
    required this.height,
    required this.area,
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
    {
      final bytes = utf8.encode(label);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
    buf.putFloat32(x);
    buf.putFloat32(y);
    buf.putFloat32(width);
    buf.putFloat32(height);
    buf.putInt32(area);
    buf.putFloat32(confidence);
  }

  static CamBlobT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static CamBlobT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final label = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();
    final x = buf.getFloat32();
    final y = buf.getFloat32();
    final width = buf.getFloat32();
    final height = buf.getFloat32();
    final area = buf.getInt32();
    final confidence = buf.getFloat32();

    return CamBlobT(
      timestamp: timestamp,
      label: label,
      x: x,
      y: y,
      width: width,
      height: height,
      area: area,
      confidence: confidence,
    );
  }
}
