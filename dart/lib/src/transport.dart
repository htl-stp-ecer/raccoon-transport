import 'dart:async';
import 'dart:typed_data';
import 'lcm/lcm.dart';
import 'lcm/lcm_buffer.dart';
import 'retain.dart';
import 'reliable.dart';

/// Options for publishing messages
class PublishOptions {
  final bool reliable;
  final bool retained;

  /// Drop byte-identical VALUE-channel payloads (command channels are never
  /// deduplicated). The decision is made in C++ (`raccoon::dedup`) on the
  /// ring transport; this flag is just forwarded.
  final bool deduplicate;
  final Duration retryInterval;
  final int maxRetries;

  const PublishOptions({
    this.reliable = false,
    this.retained = false,
    this.deduplicate = false,
    this.retryInterval = const Duration(milliseconds: 100),
    this.maxRetries = 10,
  });
}

/// Options for subscribing to messages
class SubscribeOptions {
  final bool reliable;
  final bool requestRetained;

  const SubscribeOptions({
    this.reliable = false,
    this.requestRetained = false,
  });
}

/// Main transport class wrapping LCM with optional reliable delivery
class RaccoonTransport {
  final Lcm _lcm;
  final RetainStore _retainStore = RetainStore();
  late final ReliablePublisher _reliablePublisher;
  late final ReliableSubscriber _reliableSubscriber;

  RaccoonTransport._(this._lcm) {
    final instanceId = generateInstanceId();
    _reliablePublisher = ReliablePublisher(_lcm, instanceId);
    _reliableSubscriber = ReliableSubscriber(_lcm, instanceId);
    _retainStore.startListening(_lcm);
    _reliablePublisher.startListening();
  }

  /// Create a new transport instance
  static Future<RaccoonTransport> create([String? provider]) async {
    final lcm = await Lcm.create(provider);
    return RaccoonTransport._(lcm);
  }

  /// Publish a raw message on a channel
  int publish(String channel, Uint8List data, {PublishOptions options = const PublishOptions()}) {
    if (options.reliable) {
      final bytesSent = _reliablePublisher.publish(channel, data,
          retryInterval: options.retryInterval,
          maxRetries: options.maxRetries);
      if (options.retained) {
        _retainStore.cache(channel, data);
      }
      return bytesSent;
    }
    final bytesSent = _lcm.publish(channel, data);
    if (options.retained) {
      _retainStore.cache(channel, data);
    }
    return bytesSent;
  }

  /// Publish a typed LCM message on a channel
  int publishMessage<T extends LcmMessage>(String channel, T message,
      {PublishOptions options = const PublishOptions()}) {
    // Auto-stamp `timestamp` if the caller left it at the default 0. Every
    // raccoon LCM type carries a `timestamp` field and downstream consumers
    // dedupe by it — forcing callers to set it manually has proven to be a
    // reliable foot-gun. Non-zero values are left alone so explicit /
    // replay use cases keep working. Done via dynamic dispatch because
    // the `LcmMessage` interface does not expose the field statically.
    try {
      final dyn = message as dynamic;
      if (dyn.timestamp == 0) {
        dyn.timestamp = DateTime.now().microsecondsSinceEpoch;
      }
    } catch (_) {
      // Message type does not expose a `timestamp` field — leave untouched.
    }

    // Estimate buffer size generously
    final buf = LcmBuffer(65536);
    message.encode(buf);
    final data = Uint8List.sublistView(buf.uint8List, 0, buf.position);
    return publish(channel, data, options: options);
  }

  /// Subscribe to raw messages on a channel
  LcmSubscription subscribe(String channel, LcmMessageHandler handler,
      {SubscribeOptions options = const SubscribeOptions()}) {
    if (options.reliable) {
      _reliableSubscriber.subscribe(channel, handler);
      if (options.requestRetained) {
        RetainStore.sendRequest(_lcm, channel);
      }
      // Return a dummy subscription for the user-facing channel;
      // the actual reliable subscription is managed internally
      return _lcm.subscribe('__raccoon/noop/${channel.hashCode}', (_, __) {});
    }
    final sub = _lcm.subscribe(channel, handler);
    if (options.requestRetained) {
      RetainStore.sendRequest(_lcm, channel);
    }
    return sub;
  }

  /// Unsubscribe from a channel
  void unsubscribe(LcmSubscription subscription) {
    _lcm.unsubscribe(subscription);
  }

  /// Get performance statistics
  LcmStats get stats => _lcm.stats;

  /// Close and release resources
  void dispose() {
    _reliablePublisher.dispose();
    _reliableSubscriber.dispose();
    _lcm.close();
  }
}
