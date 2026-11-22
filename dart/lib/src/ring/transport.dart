import 'dart:async';
import 'dart:ffi';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'transport_bridge_ffi.dart';

/// Outcome of a publish. The values are those of `raccoon::PublishResult` in the C++
/// transport.
enum PublishResult {
  ok,
  notInTable,
  typeMismatch,
  tooBig,
  closed,

  /// The call itself was invalid (a closed handle, a missing argument).
  invalid;

  static PublishResult fromCode(int code) =>
      code >= 0 && code < PublishResult.invalid.index
          ? PublishResult.values[code]
          : PublishResult.invalid;
}

typedef MessageHandler = void Function(String channel, Uint8List data);

/// A subscription made with [RaccoonRingTransport.subscribe].
class TransportSubscription {
  final String channel;
  final int _id;
  final MessageHandler _handler;

  TransportSubscription._(this.channel, this._id, this._handler);
}

/// The C++ transport (shared-memory rings, reliable delivery, retained channels),
/// reached through `libraccoon_transport_bridge.so`.
///
/// A channel must be in the channel table (`channels.yml`). Whether it is reliable or
/// retained is a property of the table. Frames of a subscription are queued by the bridge
/// and handed to the handlers by [spinOnce], which [startSpin] calls on a timer.
class RaccoonRingTransport {
  late final Pointer<Void> _handle;
  final Map<int, TransportSubscription> _subscriptions = {};
  Timer? _spinTimer;
  bool _disposed = false;

  Pointer<Uint8> _recvBuf = calloc<Uint8>(_initialRecvBytes);
  int _recvCapacity = _initialRecvBytes;
  final Pointer<Uint64> _recvLen = calloc<Uint64>();

  static const int _initialRecvBytes = 65536;

  RaccoonRingTransport._();

  /// [nodeName] is kept for callers that pass a name; the transport does not use it.
  static Future<RaccoonRingTransport> create([String nodeName = '']) async {
    final transport = RaccoonRingTransport._();
    final out = calloc<Pointer<Void>>();
    final ret = bridgeCreate(out);
    final handle = out.value;
    calloc.free(out);
    if (ret != 0 || handle == nullptr) {
      throw StateError('Failed to create the transport: error $ret');
    }
    transport._handle = handle;
    return transport;
  }

  /// Publish [data] on [channel].
  ///
  /// With [typeName] the channel must carry that message type, otherwise only the size of
  /// the frame is checked. When [deduplicate] is true, a byte-identical value-channel
  /// payload is dropped (command channels are never deduplicated).
  PublishResult publish(String channel, Uint8List data,
      {String? typeName, bool deduplicate = false}) {
    if (_disposed) return PublishResult.closed;
    final channelPtr = channel.toNativeUtf8();
    final typePtr = typeName?.toNativeUtf8() ?? nullptr;
    final dataPtr = calloc<Uint8>(data.isEmpty ? 1 : data.length);
    try {
      dataPtr.asTypedList(data.length).setAll(0, data);
      final code = bridgePublish(
          _handle, channelPtr, typePtr, dataPtr, data.length, deduplicate ? 1 : 0);
      return PublishResult.fromCode(code);
    } finally {
      calloc.free(dataPtr);
      calloc.free(channelPtr);
      if (typePtr != nullptr) calloc.free(typePtr);
    }
  }

  /// Subscribe to [channel]. Throws [ArgumentError] if the channel is not in the channel
  /// table or does not carry [typeName].
  TransportSubscription subscribe(String channel, MessageHandler handler,
      {String? typeName}) {
    if (_disposed) throw StateError('The transport is disposed');
    final channelPtr = channel.toNativeUtf8();
    final typePtr = typeName?.toNativeUtf8() ?? nullptr;
    final outId = calloc<Uint64>();
    try {
      final ret = bridgeSubscribe(_handle, channelPtr, typePtr, outId);
      if (ret != 0) {
        throw ArgumentError(
            'Cannot subscribe to "$channel": not in the channel table, or not of type $typeName ($ret)');
      }
      final subscription = TransportSubscription._(channel, outId.value, handler);
      _subscriptions[subscription._id] = subscription;
      return subscription;
    } finally {
      calloc.free(outId);
      calloc.free(channelPtr);
      if (typePtr != nullptr) calloc.free(typePtr);
    }
  }

  void unsubscribe(TransportSubscription subscription) {
    if (_subscriptions.remove(subscription._id) == null) return;
    if (!_disposed) bridgeUnsubscribe(_handle, subscription._id);
  }

  /// Hand every queued frame to the handlers of its subscription.
  void spinOnce() {
    if (_disposed) return;
    // Drain the whole queue of each subscription: the bridge collects frames while Dart
    // is idle between two spins, and taking one frame per spin would cap the rate at
    // the spin rate.
    for (final subscription in _subscriptions.values.toList()) {
      while (!_disposed && _subscriptions.containsKey(subscription._id)) {
        final data = _receive(subscription._id);
        if (data == null) break;
        try {
          subscription._handler(subscription.channel, data);
        } catch (_) {}
      }
    }
  }

  Uint8List? _receive(int id) {
    while (true) {
      final ret = bridgeReceive(_handle, id, _recvBuf, _recvLen, _recvCapacity);
      if (ret == 1 || ret < 0) return null;
      if (ret == 2) {
        // The frame is bigger than the buffer: take a buffer of the size it needs.
        calloc.free(_recvBuf);
        _recvCapacity = _recvLen.value;
        _recvBuf = calloc<Uint8>(_recvCapacity);
        continue;
      }
      final len = _recvLen.value;
      return Uint8List.fromList(_recvBuf.asTypedList(len));
    }
  }

  // Default spin = 33 ms (~30 fps). The bridge queues frames on its own thread; this timer
  // only hands them to Dart. Faster polling burns the UI thread without improving
  // latency, because the UI redraws at 30 fps.
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
    if (_disposed) return;
    _disposed = true;
    stopSpin();
    _subscriptions.clear();
    bridgeDestroy(_handle);
    calloc.free(_recvBuf);
    calloc.free(_recvLen);
  }
}
