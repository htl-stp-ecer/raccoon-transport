// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class EnvelopeT implements LcmMessage {

  int timestamp;
  String publisher_id;
  int seq_num;
  String channel;
  int payload_size;
  List<int> payload;

  EnvelopeT({
    required this.timestamp,
    required this.publisher_id,
    required this.seq_num,
    required this.channel,
    required this.payload_size,
    required this.payload,
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
      final bytes = utf8.encode(channel);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
    buf.putInt32(payload_size);
    for (var i0 = 0; i0 < payload_size; i0++) {
      buf.putUint8(payload[i0]);
    }
  }

  static EnvelopeT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static EnvelopeT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final publisher_id = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();
    final seq_num = buf.getInt64();
    final channel = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();
    final payload_size = buf.getInt32();
    final payload = <int>[];
    for (var i0 = 0; i0 < payload_size; i0++) {
      final payloadElement = buf.getUint8();
      payload.add(payloadElement);
    }

    return EnvelopeT(
      timestamp: timestamp,
      publisher_id: publisher_id,
      seq_num: seq_num,
      channel: channel,
      payload_size: payload_size,
      payload: payload,
    );
  }
}
