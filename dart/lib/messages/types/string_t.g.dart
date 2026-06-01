// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class StringT implements LcmMessage {

  int timestamp;
  String value;

  StringT({
    required this.timestamp,
    required this.value,
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
      final bytes = utf8.encode(value);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
  }

  static StringT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static StringT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final value = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();

    return StringT(
      timestamp: timestamp,
      value: value,
    );
  }
}
