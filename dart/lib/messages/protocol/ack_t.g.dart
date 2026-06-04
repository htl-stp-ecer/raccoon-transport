// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class AckT implements LcmMessage {

  int timestamp;
  String publisher_id;
  int seq_num;
  String subscriber_id;

  AckT({
    required this.timestamp,
    required this.publisher_id,
    required this.seq_num,
    required this.subscriber_id,
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
      final bytes = utf8.encode(publisher_id);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
    buf.putInt64(seq_num);
    {
      final bytes = utf8.encode(subscriber_id);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
  }

  static AckT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static AckT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final publisher_id = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();
    final seq_num = buf.getInt64();
    final subscriber_id = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();

    return AckT(
      timestamp: timestamp,
      publisher_id: publisher_id,
      seq_num: seq_num,
      subscriber_id: subscriber_id,
    );
  }
}
