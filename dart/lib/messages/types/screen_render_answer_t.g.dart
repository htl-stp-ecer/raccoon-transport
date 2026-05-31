// Hand-adapted from the lcm-dart generated version to match
// raccoon-lib's hand-rolled `ScreenRenderAnswer` raw byte encoding.
// No LCM fingerprint, no null-terminator. See screen_render_t.g.dart
// header comment for the wire layout.

import 'dart:convert';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

class ScreenRenderAnswerT implements LcmMessage {
  int timestamp;
  String screen_name;
  String value;
  String reason;

  ScreenRenderAnswerT({
    required this.timestamp,
    required this.screen_name,
    required this.value,
    required this.reason,
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
    void putStr(String s) {
      final bytes = utf8.encode(s);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }

    putStr(screen_name);
    putStr(value);
    putStr(reason);
  }

  static ScreenRenderAnswerT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static ScreenRenderAnswerT decodeBody(LcmBuffer buf) {
    String readStr() {
      final len = buf.getUint32();
      final bytes = buf.getUint8List(len);
      return utf8.decode(bytes);
    }

    final timestamp = buf.getInt64();
    final screenName = readStr();
    final value = readStr();
    final reason = readStr();

    return ScreenRenderAnswerT(
      timestamp: timestamp,
      screen_name: screenName,
      value: value,
      reason: reason,
    );
  }
}
