// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';
import 'yolo_box_t.g.dart';

class YoloFrameT implements LcmMessage {

  int timestamp;
  int frame_width;
  int frame_height;
  int frame_size;
  List<int> frame_data;
  int num_boxes;
  List<YoloBoxT> boxes;

  YoloFrameT({
    required this.timestamp,
    required this.frame_width,
    required this.frame_height,
    required this.frame_size,
    required this.frame_data,
    required this.num_boxes,
    required this.boxes,
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
    buf.putInt32(frame_size);
    for (var i0 = 0; i0 < frame_size; i0++) {
      buf.putUint8(frame_data[i0]);
    }
    buf.putInt32(num_boxes);
    for (var i0 = 0; i0 < num_boxes; i0++) {
      boxes[i0].encodeBody(buf);
    }
  }

  static YoloFrameT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static YoloFrameT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final frame_width = buf.getInt32();
    final frame_height = buf.getInt32();
    final frame_size = buf.getInt32();
    final frame_data = <int>[];
    for (var i0 = 0; i0 < frame_size; i0++) {
      final frame_dataElement = buf.getUint8();
      frame_data.add(frame_dataElement);
    }
    final num_boxes = buf.getInt32();
    final boxes = <YoloBoxT>[];
    for (var i0 = 0; i0 < num_boxes; i0++) {
      final boxesElement = YoloBoxT.decodeBody(buf);
      boxes.add(boxesElement);
    }

    return YoloFrameT(
      timestamp: timestamp,
      frame_width: frame_width,
      frame_height: frame_height,
      frame_size: frame_size,
      frame_data: frame_data,
      num_boxes: num_boxes,
      boxes: boxes,
    );
  }
}
