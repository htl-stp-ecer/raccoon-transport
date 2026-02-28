import 'dart:async';
import 'dart:typed_data';
import 'lcm/lcm.dart';
import 'lcm/lcm_buffer.dart';

/// Options for publishing messages
class PublishOptions {
  final bool reliable;
  final bool retained;
  final Duration retryInterval;
  final int maxRetries;

  const PublishOptions({
    this.reliable = false,
    this.retained = false,
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

  RaccoonTransport._(this._lcm);

  /// Create a new transport instance
  static Future<RaccoonTransport> create([String? provider]) async {
    final lcm = await Lcm.create(provider);
    return RaccoonTransport._(lcm);
  }

  /// Publish a raw message on a channel
  int publish(String channel, Uint8List data, {PublishOptions options = const PublishOptions()}) {
    if (options.reliable || options.retained) {
      // ignore: avoid_print
      print('raccoon_transport: reliable/retained not yet implemented, '
          'falling back to plain publish on: $channel');
    }
    return _lcm.publish(channel, data);
  }

  /// Publish a typed LCM message on a channel
  int publishMessage<T extends LcmMessage>(String channel, T message,
      {PublishOptions options = const PublishOptions()}) {
    // Estimate buffer size generously
    final buf = LcmBuffer(65536);
    message.encode(buf);
    final data = Uint8List.sublistView(buf.uint8List, 0, buf.position);
    return publish(channel, data, options: options);
  }

  /// Subscribe to raw messages on a channel
  LcmSubscription subscribe(String channel, LcmMessageHandler handler,
      {SubscribeOptions options = const SubscribeOptions()}) {
    if (options.reliable || options.requestRetained) {
      // ignore: avoid_print
      print('raccoon_transport: reliable/retained not yet implemented, '
          'falling back to plain subscribe on: $channel');
    }
    return _lcm.subscribe(channel, handler);
  }

  /// Unsubscribe from a channel
  void unsubscribe(LcmSubscription subscription) {
    _lcm.unsubscribe(subscription);
  }

  /// Get performance statistics
  LcmStats get stats => _lcm.stats;

  /// Close and release resources
  void dispose() {
    _lcm.close();
  }
}
