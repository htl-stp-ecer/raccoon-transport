import 'dart:ffi';
import 'package:ffi/ffi.dart';

String? _customLibraryPath;

/// Directory that holds `libraccoon_transport_bridge.so`.
void setLibraryPath(String path) {
  _customLibraryPath = path;
}

DynamicLibrary _loadLibrary() {
  const libName = 'libraccoon_transport_bridge.so';
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
      'Call setLibraryPath() to specify the directory containing the .so file.');
}

final DynamicLibrary _lib = _loadLibrary();

typedef _CreateNative = Int32 Function(Pointer<Pointer<Void>> outTransport);
typedef _CreateDart = int Function(Pointer<Pointer<Void>> outTransport);

typedef _DestroyNative = Void Function(Pointer<Void> transport);
typedef _DestroyDart = void Function(Pointer<Void> transport);

typedef _PublishNative = Int32 Function(Pointer<Void> transport,
    Pointer<Utf8> channel, Pointer<Utf8> typeName, Pointer<Uint8> data,
    Uint64 len, Int32 deduplicate);
typedef _PublishDart = int Function(Pointer<Void> transport,
    Pointer<Utf8> channel, Pointer<Utf8> typeName, Pointer<Uint8> data,
    int len, int deduplicate);

typedef _SubscribeNative = Int32 Function(Pointer<Void> transport,
    Pointer<Utf8> channel, Pointer<Utf8> typeName, Pointer<Uint64> outId);
typedef _SubscribeDart = int Function(Pointer<Void> transport,
    Pointer<Utf8> channel, Pointer<Utf8> typeName, Pointer<Uint64> outId);

typedef _UnsubscribeNative = Void Function(Pointer<Void> transport, Uint64 id);
typedef _UnsubscribeDart = void Function(Pointer<Void> transport, int id);

typedef _ReceiveNative = Int32 Function(Pointer<Void> transport, Uint64 id,
    Pointer<Uint8> buf, Pointer<Uint64> outLen, Uint64 maxLen);
typedef _ReceiveDart = int Function(Pointer<Void> transport, int id,
    Pointer<Uint8> buf, Pointer<Uint64> outLen, int maxLen);

final _CreateDart bridgeCreate = _lib
    .lookupFunction<_CreateNative, _CreateDart>('raccoon_transport_create');
final _DestroyDart bridgeDestroy = _lib
    .lookupFunction<_DestroyNative, _DestroyDart>('raccoon_transport_destroy');
final _PublishDart bridgePublish = _lib
    .lookupFunction<_PublishNative, _PublishDart>('raccoon_transport_publish');
final _SubscribeDart bridgeSubscribe = _lib.lookupFunction<_SubscribeNative,
    _SubscribeDart>('raccoon_transport_subscribe');
final _UnsubscribeDart bridgeUnsubscribe = _lib.lookupFunction<
    _UnsubscribeNative, _UnsubscribeDart>('raccoon_transport_unsubscribe');
final _ReceiveDart bridgeReceive = _lib
    .lookupFunction<_ReceiveNative, _ReceiveDart>('raccoon_transport_receive');
