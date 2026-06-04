// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class RetainRequestT implements LcmMessage {

  int timestamp;
  String channel;
  String subscriber_id;

  RetainRequestT({
    required this.timestamp,
    required this.channel,
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
      final bytes = utf8.encode(channel);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
    {
      final bytes = utf8.encode(subscriber_id);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
  }

  static RetainRequestT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static RetainRequestT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final channel = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();
    final subscriber_id = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();

    return RetainRequestT(
      timestamp: timestamp,
      channel: channel,
      subscriber_id: subscriber_id,
    );
  }
}
