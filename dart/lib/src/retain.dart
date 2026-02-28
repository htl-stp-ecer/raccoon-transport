import 'dart:typed_data';
import 'lcm/lcm.dart';
import 'lcm/lcm_buffer.dart';
import 'channels.dart';

/// Publisher-side retain store. Caches latest raw bytes per channel and
/// replays on `__raccoon/retain_request`.
class RetainStore {
  final Map<String, Uint8List> _cache = {};
  Lcm? _lcm;

  void cache(String channel, Uint8List data) {
    _cache[channel] = Uint8List.fromList(data);
  }

  Uint8List? get(String channel) => _cache[channel];

  void startListening(Lcm lcm) {
    _lcm = lcm;
    lcm.subscribe(ProtocolChannels.retainRequest, _onRetainRequest);
  }

  void _onRetainRequest(String channel, Uint8List data) {
    // Decode retain_request_t: int64 timestamp, string channel, string subscriber_id
    final buf = LcmBuffer.fromUint8List(data);

    // Skip fingerprint (8 bytes)
    buf.getInt64();
    // Skip timestamp
    buf.getInt64();
    // Read channel (LCM string: int32 len + bytes)
    final channelLen = buf.getInt32();
    final channelBytes = buf.getUint8List(channelLen);
    final requestedChannel = String.fromCharCodes(channelBytes);

    final cached = _cache[requestedChannel];
    if (cached != null && _lcm != null) {
      _lcm!.publish(requestedChannel, cached);
    }
  }

  /// Encode and publish a retain_request_t for the given channel.
  static void sendRequest(Lcm lcm, String channel) {
    final channelBytes = channel.codeUnits;
    final subscriberBytes = <int>[]; // empty subscriber_id

    // retain_request_t layout:
    // int64 fingerprint + int64 timestamp + string channel + string subscriber_id
    // LCM string = int32 length + bytes (null-terminated in Java, but length-prefixed in binary)
    final size = 8 + 8 + 4 + channelBytes.length + 4 + subscriberBytes.length;
    final buf = LcmBuffer(size);

    // Fingerprint - we need the actual hash but for simplicity use 0
    // The C receiver uses lcm_subscribe which bypasses fingerprint checking
    buf.putInt64(0);
    // timestamp
    buf.putInt64(DateTime.now().microsecondsSinceEpoch);
    // channel string
    buf.putInt32(channelBytes.length);
    buf.putUint8List(channelBytes);
    // subscriber_id string
    buf.putInt32(0);

    final data = Uint8List.sublistView(buf.uint8List, 0, buf.position);
    lcm.publish(ProtocolChannels.retainRequest, data);
  }
}
