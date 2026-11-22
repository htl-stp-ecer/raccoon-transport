// Hand-written transport message: a title and a text for the user.
// Raw big-endian binary, strings with a 32-bit length and no terminator.

import 'dart:convert';
import 'package:raccoon_transport/raccoon_transport.dart';

class MessageT implements LcmMessage {

  int timestamp;
  String title;
  String content;

  MessageT({
    required this.timestamp,
    required this.title,
    required this.content,
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
    for (final text in [title, content]) {
      final bytes = utf8.encode(text);
      buf.putUint32(bytes.length);
      buf.putUint8List(bytes);
    }
  }

  static MessageT decode(LcmBuffer buf) {
    return decodeBody(buf);
  }

  static MessageT decodeBody(LcmBuffer buf) {
    final timestamp = buf.getInt64();
    String readString() {
      final len = buf.getUint32();
      return utf8.decode(buf.getUint8List(len));
    }
    final title = readString();
    final content = readString();

    return MessageT(
      timestamp: timestamp,
      title: title,
      content: content,
    );
  }
}
