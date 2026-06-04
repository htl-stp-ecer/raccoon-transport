import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';

String? _customLibraryPath;

void setLibraryPath(String path) {
  _customLibraryPath = path;
}

DynamicLibrary _loadLibrary() {
  const libName = 'libraccoon_ring_bridge.so';
  final pathsToTry = <String>[];

  if (_customLibraryPath != null) {
    pathsToTry.add('$_customLibraryPath/$libName');
  }

  pathsToTry.addAll([
    '/home/pi/stp-velox/$libName',
    libName,
  ]);

  for (final path in pathsToTry) {
    try {
      return DynamicLibrary.open(path);
    } catch (_) {
      continue;
    }
  }

  throw UnsupportedError(
      'Could not load $libName. Tried: ${pathsToTry.join(", ")}. '
      'Call setLibraryPath() to specify the directory containing the .so files.');
}

final DynamicLibrary _lib = _loadLibrary();

final class _Bridge {}

typedef _NodeCreateNative = Int8 Function(
    Pointer<Pointer<Void>> outNode, Pointer<Utf8> name);
typedef _NodeCreateDart = int Function(
    Pointer<Pointer<Void>> outNode, Pointer<Utf8> name);

typedef _NodeDestroyNative = Void Function(Pointer<Void> node);
typedef _NodeDestroyDart = void Function(Pointer<Void> node);

typedef _PubCreateNative = Int8 Function(
    Pointer<Void> node, Pointer<Utf8> channel, Pointer<Pointer<Void>> outPub);
typedef _PubCreateDart = int Function(
    Pointer<Void> node, Pointer<Utf8> channel, Pointer<Pointer<Void>> outPub);

typedef _PubSendNative = Int8 Function(
    Pointer<Void> pub, Pointer<Uint8> data, Uint64 len);
typedef _PubSendDart = int Function(
    Pointer<Void> pub, Pointer<Uint8> data, int len);

typedef _PubDestroyNative = Void Function(Pointer<Void> pub);
typedef _PubDestroyDart = void Function(Pointer<Void> pub);

typedef _SubCreateNative = Int8 Function(
    Pointer<Void> node, Pointer<Utf8> channel, Pointer<Pointer<Void>> outSub);
typedef _SubCreateDart = int Function(
    Pointer<Void> node, Pointer<Utf8> channel, Pointer<Pointer<Void>> outSub);

typedef _SubReceiveNative = Int8 Function(
    Pointer<Void> sub, Pointer<Uint8> buf, Pointer<Uint64> outLen, Uint64 maxLen);
typedef _SubReceiveDart = int Function(
    Pointer<Void> sub, Pointer<Uint8> buf, Pointer<Uint64> outLen, int maxLen);

typedef _SubDestroyNative = Void Function(Pointer<Void> sub);
typedef _SubDestroyDart = void Function(Pointer<Void> sub);

final _NodeCreateDart _nodeCreate = _lib
    .lookupFunction<_NodeCreateNative, _NodeCreateDart>('raccoon_ring_bridge_node_create');

final _NodeDestroyDart _nodeDestroy = _lib
    .lookupFunction<_NodeDestroyNative, _NodeDestroyDart>('raccoon_ring_bridge_node_destroy');

final _PubCreateDart _pubCreate = _lib
    .lookupFunction<_PubCreateNative, _PubCreateDart>('raccoon_ring_bridge_publisher_create');

final _PubSendDart _pubSend = _lib
    .lookupFunction<_PubSendNative, _PubSendDart>('raccoon_ring_bridge_publisher_send');

final _PubDestroyDart _pubDestroy = _lib
    .lookupFunction<_PubDestroyNative, _PubDestroyDart>('raccoon_ring_bridge_publisher_destroy');

final _SubCreateDart _subCreate = _lib
    .lookupFunction<_SubCreateNative, _SubCreateDart>('raccoon_ring_bridge_subscriber_create');

final _SubReceiveDart _subReceive = _lib
    .lookupFunction<_SubReceiveNative, _SubReceiveDart>('raccoon_ring_bridge_subscriber_receive');

final _SubDestroyDart _subDestroy = _lib
    .lookupFunction<_SubDestroyNative, _SubDestroyDart>('raccoon_ring_bridge_subscriber_destroy');

class RingNode {
  final Pointer<Void> _handle;

  RingNode._(this._handle);

  factory RingNode(String name) {
    final namePtr = name.toNativeUtf8();
    final outPtr = calloc<Pointer<Void>>();
    final ret = _nodeCreate(outPtr, namePtr);
    calloc.free(namePtr);
    if (ret != 0) {
      final h = outPtr.value;
      calloc.free(outPtr);
      throw Exception('Failed to create node "$name": error code $ret');
    }
    final h = outPtr.value;
    calloc.free(outPtr);
    if (h == nullptr) {
      throw Exception('Failed to create node "$name": null handle');
    }
    return RingNode._(h);
  }

  Pointer<Void> get handle => _handle;

  void dispose() {
    _nodeDestroy(_handle);
  }
}

class RingPublisher {
  final Pointer<Void> _handle;

  RingPublisher._(this._handle);

  factory RingPublisher(RingNode node, String channel) {
    final chPtr = channel.toNativeUtf8();
    final outPtr = calloc<Pointer<Void>>();
    final ret = _pubCreate(node.handle, chPtr, outPtr);
    calloc.free(chPtr);
    if (ret != 0) {
      final h = outPtr.value;
      calloc.free(outPtr);
      throw Exception('Failed to create publisher for "$channel": error $ret');
    }
    final h = outPtr.value;
    calloc.free(outPtr);
    if (h == nullptr) {
      throw Exception('Failed to create publisher for "$channel": null handle');
    }
    return RingPublisher._(h);
  }

  Pointer<Void> get handle => _handle;

  int send(Uint8List data) {
    final dataLen = data.length;
    final dataPtr = calloc<Uint8>(dataLen);
    for (var i = 0; i < dataLen; i++) {
      dataPtr[i] = data[i];
    }
    final ret = _pubSend(_handle, dataPtr, dataLen);
    calloc.free(dataPtr);
    return ret;
  }

  void dispose() {
    _pubDestroy(_handle);
  }
}

class RingSubscriber {
  final Pointer<Void> _handle;
  static const int _recvBufSize = 65536;
  final Pointer<Uint8> _recvBuf;
  final Pointer<Uint64> _recvLen;

  RingSubscriber._(this._handle)
      : _recvBuf = calloc<Uint8>(_recvBufSize),
        _recvLen = calloc<Uint64>();

  factory RingSubscriber(RingNode node, String channel) {
    final chPtr = channel.toNativeUtf8();
    final outPtr = calloc<Pointer<Void>>();
    final ret = _subCreate(node.handle, chPtr, outPtr);
    calloc.free(chPtr);
    if (ret != 0) {
      final h = outPtr.value;
      calloc.free(outPtr);
      throw Exception('Failed to create subscriber for "$channel": error $ret');
    }
    final h = outPtr.value;
    calloc.free(outPtr);
    if (h == nullptr) {
      throw Exception('Failed to create subscriber for "$channel": null handle');
    }
    return RingSubscriber._(h);
  }

  Pointer<Void> get handle => _handle;

  Uint8List? receive() {
    final ret = _subReceive(_handle, _recvBuf, _recvLen, _recvBufSize);
    if (ret == 1) return null;
    if (ret != 0) {
      throw Exception('Subscriber receive error: $ret');
    }
    final len = _recvLen.value;
    if (len == 0) return null;
    final data = Uint8List(len);
    for (var i = 0; i < len; i++) {
      data[i] = _recvBuf[i];
    }
    return data;
  }

  void dispose() {
    calloc.free(_recvBuf);
    calloc.free(_recvLen);
    _subDestroy(_handle);
  }
}
