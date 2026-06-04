// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class CamConfigT implements LcmMessage {

  int timestamp;
  String config;

  CamConfigT({
    required this.timestamp,
    required this.config,
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
      final bytes = utf8.encode(config);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
  }

  static CamConfigT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static CamConfigT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final config = () {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }();

    return CamConfigT(
      timestamp: timestamp,
      config: config,
    );
  }
}
