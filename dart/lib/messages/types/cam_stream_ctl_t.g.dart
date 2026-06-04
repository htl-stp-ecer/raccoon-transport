// Hand-written transport message (was lcm-dart generated).
// No LCM fingerprint, no string null terminator — raw BE binary.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class CamStreamCtlT implements LcmMessage {

  int timestamp;
  int enabled;

  CamStreamCtlT({
    required this.timestamp,
    required this.enabled,
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
    buf.putInt8(enabled);
  }

  static CamStreamCtlT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static CamStreamCtlT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    final enabled = buf.getInt8();

    return CamStreamCtlT(
      timestamp: timestamp,
      enabled: enabled,
    );
  }
}
