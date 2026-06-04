// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';
import 'cam_blob_t.g.dart';

class CamDetectionsT implements LcmMessage {

  int timestamp;
  int frame_width;
  int frame_height;
  int num_detections;
  List<CamBlobT> detections;

  CamDetectionsT({
    required this.timestamp,
    required this.frame_width,
    required this.frame_height,
    required this.num_detections,
    required this.detections,
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
    buf.putInt32(frame_width);
    buf.putInt32(frame_height);
    buf.putInt32(num_detections);
    for (var i0 = 0; i0 < num_detections; i0++) {
      detections[i0].encodeBody(buf);
    }
  }

  static CamDetectionsT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static CamDetectionsT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final frame_width = buf.getInt32();
    final frame_height = buf.getInt32();
    final num_detections = buf.getInt32();
    final detections = <CamBlobT>[];
    for (var i0 = 0; i0 < num_detections; i0++) {
      final detectionsElement = CamBlobT.decodeBody(buf);
      detections.add(detectionsElement);
    }

    return CamDetectionsT(
      timestamp: timestamp,
      frame_width: frame_width,
      frame_height: frame_height,
      num_detections: num_detections,
      detections: detections,
    );
  }
}
