import 'dart:async';
import 'dart:typed_data';

import 'raccoon_ring_bridge_ffi.dart';

class TransportSubscription {
  final String channel;
  final MessageHandler _handler;

  TransportSubscription._(this.channel, this._handler);

  bool _matchesHandler(MessageHandler handler) =>
      identical(_handler, handler) || handler == _handler;
}

typedef MessageHandler = void Function(String channel, Uint8List data);

class RaccoonRingTransport {
  late final RingNode _node;
  final Map<String, RingPublisher> _publishers = {};
  final Map<String, _SubEntry> _subscribers = {};
  Timer? _spinTimer;
  bool _disposed = false;

  RaccoonRingTransport._();

  static Future<RaccoonRingTransport> create(String nodeName) async {
    final transport = RaccoonRingTransport._();
    transport._node = RingNode(nodeName);
    return transport;
  }

  /// Publish [data] on [channel].
  ///
  /// When [deduplicate] is true, a byte-identical value-channel payload is
  /// dropped by the C++ bridge (command channels are never deduplicated —
  /// see `raccoon::dedup`). The dedup decision lives in C++; this only
  /// forwards the flag.
  int publish(String channel, Uint8List data, {bool deduplicate = false}) {
    final pub = _publishers.putIfAbsent(
        channel, () => RingPublisher(_node, channel));
    return pub.send(data, deduplicate: deduplicate);
  }

  TransportSubscription subscribe(String channel, MessageHandler handler) {
    final entry = _subscribers.putIfAbsent(channel, () {
      final sub = RingSubscriber(_node, channel);
      return _SubEntry(sub, channel);
    });

    entry._addHandler(handler);
    return TransportSubscription._(channel, handler);
  }

  void unsubscribe(TransportSubscription subscription) {
    final entry = _subscribers[subscription.channel];
    if (entry == null) return;

    entry._removeHandler(subscription._handler);

    if (entry._handlers.isEmpty) {
      _subscribers.remove(subscription.channel);
      entry.sub.dispose();
    }
  }

  void spinOnce() {
    if (_disposed) return;

    // Drain every queued frame per channel rather than just one. The
    // bridge poll thread accumulates frames into a deque while Dart is
    // idle between spins; if we only pop one per spin we cap delivery
    // at intervalMs⁻¹ Hz no matter how fast the publisher actually runs
    // (e.g. analog at 200 Hz vs 100 Hz spin = 50 % loss). With
    // drain-per-spin we keep up regardless of spin interval.
    for (final entry in _subscribers.values.toList()) {
      while (true) {
        Uint8List? data;
        try {
          data = entry.sub.receive();
        } catch (_) {
          break;
        }
        if (data == null) break;
        for (final handler in entry._handlers.toList()) {
          try {
            handler(entry._channel, data);
          } catch (_) {}
        }
      }
    }
  }

  // Default spin = 33 ms (~30 fps). The C++ bridge thread is already
  // futex-woken on every publish and queues frames; this timer only
  // drains the queue into Dart streams. Faster polling here just
  // burns the UI thread without improving end-to-end latency — the
  // UI itself only redraws at 30 fps.
  void startSpin({int intervalMs = 33}) {
    _spinTimer?.cancel();
    _spinTimer = Timer.periodic(Duration(milliseconds: intervalMs), (_) {
      spinOnce();
    });
  }

  void stopSpin() {
    _spinTimer?.cancel();
    _spinTimer = null;
  }

  void dispose() {
    _disposed = true;
    stopSpin();
    for (final pub in _publishers.values) {
      pub.dispose();
    }
    _publishers.clear();
    for (final entry in _subscribers.values) {
      entry.sub.dispose();
    }
    _subscribers.clear();
    _node.dispose();
  }
}

class _SubEntry {
  final RingSubscriber sub;
  final String _channel;
  final List<MessageHandler> _handlers = [];

  _SubEntry(this.sub, this._channel);

  void _addHandler(MessageHandler handler) {
    _handlers.add(handler);
  }

  void _removeHandler(MessageHandler handler) {
    _handlers.removeWhere((h) => identical(h, handler) || h == handler);
  }
}
