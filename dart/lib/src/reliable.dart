import 'dart:async';
import 'dart:math';
import 'dart:typed_data';
import 'lcm/lcm.dart';
import 'lcm/lcm_buffer.dart';
import 'channels.dart';

String generateInstanceId() {
  final rng = Random.secure();
  final bytes = List.generate(8, (_) => rng.nextInt(256));
  return bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
}

/// Encodes an envelope_t into bytes matching the LCM-generated format.
Uint8List _encodeEnvelope({
  required int timestamp,
  required String publisherId,
  required int seqNum,
  required String channel,
  required Uint8List payload,
}) {
  final pubIdBytes = publisherId.codeUnits;
  final channelBytes = channel.codeUnits;
  // fingerprint(8) + timestamp(8) + string publisher_id(4+len+1) +
  // seq_num(8) + string channel(4+len+1) + payload_size(4) + payload
  final size = 8 + 8 + 4 + pubIdBytes.length + 1 + 8 + 4 +
      channelBytes.length + 1 + 4 + payload.length;
  final buf = LcmBuffer(size);

  // Fingerprint: envelope_t hash = (0xe68bfc03c6cb3940 << 1) | (top bit)
  // = 0xcd17f8078d967281
  buf.putInt64(0xcd17f8078d967281);

  buf.putInt64(timestamp);

  buf.putInt32(pubIdBytes.length + 1);
  buf.putUint8List(pubIdBytes);
  buf.putUint8(0);

  buf.putInt64(seqNum);

  buf.putInt32(channelBytes.length + 1);
  buf.putUint8List(channelBytes);
  buf.putUint8(0);

  buf.putInt32(payload.length);
  buf.putUint8List(payload);

  return Uint8List.sublistView(buf.uint8List, 0, buf.position);
}

/// Decodes an envelope_t from bytes.
({int timestamp, String publisherId, int seqNum, String channel, Uint8List payload})?
    _decodeEnvelope(Uint8List data) {
  try {
    final buf = LcmBuffer.fromUint8List(data);

    // Skip fingerprint
    buf.getInt64();

    final timestamp = buf.getInt64();

    final pubIdLen = buf.getInt32();
    final pubIdBytes = buf.getUint8List(pubIdLen - 1);
    buf.getUint8(); // null terminator
    final publisherId = String.fromCharCodes(pubIdBytes);

    final seqNum = buf.getInt64();

    final channelLen = buf.getInt32();
    final channelBytes = buf.getUint8List(channelLen - 1);
    buf.getUint8(); // null terminator
    final channel = String.fromCharCodes(channelBytes);

    final payloadSize = buf.getInt32();
    final payload = buf.getUint8List(payloadSize);

    return (
      timestamp: timestamp,
      publisherId: publisherId,
      seqNum: seqNum,
      channel: channel,
      payload: payload,
    );
  } catch (_) {
    return null;
  }
}

/// Encodes an ack_t into bytes matching the LCM-generated format.
Uint8List _encodeAck({
  required int timestamp,
  required String publisherId,
  required int seqNum,
  required String subscriberId,
}) {
  final pubIdBytes = publisherId.codeUnits;
  final subIdBytes = subscriberId.codeUnits;
  // fingerprint(8) + timestamp(8) + string publisher_id(4+len+1) +
  // seq_num(8) + string subscriber_id(4+len+1)
  final size =
      8 + 8 + 4 + pubIdBytes.length + 1 + 8 + 4 + subIdBytes.length + 1;
  final buf = LcmBuffer(size);

  // Fingerprint: ack_t hash = (0xac3540153e27ca42 << 1) | (top bit)
  // = 0x586a802a7c4f9485
  buf.putInt64(0x586a802a7c4f9485 & 0x7fffffffffffffff); // as signed

  buf.putInt64(timestamp);

  buf.putInt32(pubIdBytes.length + 1);
  buf.putUint8List(pubIdBytes);
  buf.putUint8(0);

  buf.putInt64(seqNum);

  buf.putInt32(subIdBytes.length + 1);
  buf.putUint8List(subIdBytes);
  buf.putUint8(0);

  return Uint8List.sublistView(buf.uint8List, 0, buf.position);
}

/// Decodes an ack_t from bytes.
({int timestamp, String publisherId, int seqNum, String subscriberId})?
    _decodeAck(Uint8List data) {
  try {
    final buf = LcmBuffer.fromUint8List(data);

    // Skip fingerprint
    buf.getInt64();

    final timestamp = buf.getInt64();

    final pubIdLen = buf.getInt32();
    final pubIdBytes = buf.getUint8List(pubIdLen - 1);
    buf.getUint8();
    final publisherId = String.fromCharCodes(pubIdBytes);

    final seqNum = buf.getInt64();

    final subIdLen = buf.getInt32();
    final subIdBytes = buf.getUint8List(subIdLen - 1);
    buf.getUint8();
    final subscriberId = String.fromCharCodes(subIdBytes);

    return (
      timestamp: timestamp,
      publisherId: publisherId,
      seqNum: seqNum,
      subscriberId: subscriberId,
    );
  } catch (_) {
    return null;
  }
}

class _PendingMessage {
  final String reliableChannel;
  final Uint8List envelopeData;
  final Duration retryInterval;
  final int maxRetries;
  final int seqNum;
  DateTime lastSent;
  int attempts = 1;

  _PendingMessage({
    required this.reliableChannel,
    required this.envelopeData,
    required this.retryInterval,
    required this.maxRetries,
    required this.seqNum,
    required this.lastSent,
  });
}

/// Handles reliable publishing with retransmission until ACK received.
class ReliablePublisher {
  final String instanceId;
  final Lcm _lcm;
  int _seqNum = 0;
  final List<_PendingMessage> _pending = [];
  Timer? _retryTimer;
  LcmSubscription? _ackSub;

  ReliablePublisher(this._lcm, this.instanceId);

  void startListening() {
    _ackSub = _lcm.subscribe(ProtocolChannels.ack, _onAck);
  }

  int publish(String channel, Uint8List data,
      {Duration retryInterval = const Duration(milliseconds: 100),
      int maxRetries = 10}) {
    final seqNum = _seqNum++;
    final now = DateTime.now();
    final envData = _encodeEnvelope(
      timestamp: now.microsecondsSinceEpoch,
      publisherId: instanceId,
      seqNum: seqNum,
      channel: channel,
      payload: data,
    );

    final reliableChannel = ProtocolChannels.reliableChannel(channel);
    final bytesSent = _lcm.publish(reliableChannel, envData);

    _pending.add(_PendingMessage(
      reliableChannel: reliableChannel,
      envelopeData: envData,
      retryInterval: retryInterval,
      maxRetries: maxRetries,
      seqNum: seqNum,
      lastSent: now,
    ));

    _ensureTimer();
    return bytesSent;
  }

  void _onAck(String channel, Uint8List data) {
    final ack = _decodeAck(data);
    if (ack == null) return;
    if (ack.publisherId != instanceId) return;

    _pending.removeWhere((msg) => msg.seqNum == ack.seqNum);
    if (_pending.isEmpty) {
      _retryTimer?.cancel();
      _retryTimer = null;
    }
  }

  void _ensureTimer() {
    if (_retryTimer != null) return;
    _retryTimer = Timer.periodic(const Duration(milliseconds: 50), (_) {
      _tick();
    });
  }

  void _tick() {
    final now = DateTime.now();
    _pending.removeWhere((msg) {
      if (now.difference(msg.lastSent) >= msg.retryInterval) {
        if (msg.attempts >= msg.maxRetries) {
          // ignore: avoid_print
          print('raccoon_transport: max retries exhausted for seq=${msg.seqNum} '
              'on ${msg.reliableChannel}');
          return true; // remove
        }
        _lcm.publish(msg.reliableChannel, msg.envelopeData);
        msg.lastSent = now;
        msg.attempts++;
      }
      return false;
    });

    if (_pending.isEmpty) {
      _retryTimer?.cancel();
      _retryTimer = null;
    }
  }

  void dispose() {
    _retryTimer?.cancel();
    _retryTimer = null;
    if (_ackSub != null) {
      _lcm.unsubscribe(_ackSub!);
      _ackSub = null;
    }
  }
}

/// Handles reliable subscribing with deduplication and ACK sending.
class ReliableSubscriber {
  final String instanceId;
  final Lcm _lcm;
  final List<String> _dedupRing = [];
  final Set<String> _dedupSet = {};
  final List<LcmSubscription> _subscriptions = [];

  static const int _dedupCapacity = 1000;

  ReliableSubscriber(this._lcm, this.instanceId);

  void subscribe(String channel, LcmMessageHandler handler) {
    final reliableChannel = ProtocolChannels.reliableChannel(channel);
    final sub = _lcm.subscribe(reliableChannel, (_, data) {
      _onEnvelope(channel, data, handler);
    });
    _subscriptions.add(sub);
  }

  void _onEnvelope(
      String expectedChannel, Uint8List data, LcmMessageHandler handler) {
    final env = _decodeEnvelope(data);
    if (env == null) return;
    if (env.channel != expectedChannel) return;

    // Send ACK
    final ackData = _encodeAck(
      timestamp: DateTime.now().microsecondsSinceEpoch,
      publisherId: env.publisherId,
      seqNum: env.seqNum,
      subscriberId: instanceId,
    );
    _lcm.publish(ProtocolChannels.ack, ackData);

    // Deduplicate
    final key = '${env.publisherId}:${env.seqNum}';
    if (_dedupSet.contains(key)) return;

    if (_dedupRing.length >= _dedupCapacity) {
      _dedupSet.remove(_dedupRing.removeAt(0));
    }
    _dedupRing.add(key);
    _dedupSet.add(key);

    // Deliver
    handler(expectedChannel, env.payload);
  }

  void dispose() {
    for (final sub in _subscriptions) {
      _lcm.unsubscribe(sub);
    }
    _subscriptions.clear();
  }
}
