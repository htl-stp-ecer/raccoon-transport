import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

void _printEvent(Map<String, dynamic> event) {
  stdout.writeln(jsonEncode(event));
}

LcmMessage _decodeMessage(String type, Uint8List data) {
  final buf = LcmBuffer.fromUint8List(data);
  switch (type) {
    case 'scalar_f_t':
      return ScalarFT.decode(buf);
    case 'scalar_i32_t':
      return ScalarI32T.decode(buf);
    case 'vector3f_t':
      return Vector3fT.decode(buf);
    case 'string_t':
      return StringT.decode(buf);
    default:
      throw ArgumentError('Unknown type: $type');
  }
}

Map<String, dynamic> _messageToJson(String type, LcmMessage msg) {
  switch (type) {
    case 'scalar_f_t':
      final m = msg as ScalarFT;
      return {'timestamp': m.timestamp, 'value': m.value};
    case 'scalar_i32_t':
      final m = msg as ScalarI32T;
      return {'timestamp': m.timestamp, 'value': m.value};
    case 'vector3f_t':
      final m = msg as Vector3fT;
      return {'timestamp': m.timestamp, 'x': m.x, 'y': m.y, 'z': m.z};
    case 'string_t':
      final m = msg as StringT;
      return {'timestamp': m.timestamp, 'value': m.value};
    default:
      throw ArgumentError('Unknown type: $type');
  }
}

int _getFingerprint(String type) {
  switch (type) {
    case 'scalar_f_t':
      return ScalarFT.LCM_FINGERPRINT;
    case 'scalar_i32_t':
      return ScalarI32T.LCM_FINGERPRINT;
    case 'vector3f_t':
      return Vector3fT.LCM_FINGERPRINT;
    case 'string_t':
      return StringT.LCM_FINGERPRINT;
    default:
      throw ArgumentError('Unknown type: $type');
  }
}

Future<void> main(List<String> args) async {
  // --fingerprint <type>: print fingerprint and exit
  if (args.isNotEmpty && args[0] == '--fingerprint') {
    final type = args[1];
    final fp = _getFingerprint(type);
    final hex =
        '0x${BigInt.from(fp).toUnsigned(64).toRadixString(16).padLeft(16, '0')}';
    _printEvent({'event': 'fingerprint', 'value': hex});
    exit(0);
  }

  // --decode-hex <type> <hex>: decode hex bytes and exit
  if (args.isNotEmpty && args[0] == '--decode-hex') {
    final type = args[1];
    final hexStr = args[2];
    final bytes = Uint8List(hexStr.length ~/ 2);
    for (int i = 0; i < bytes.length; i++) {
      bytes[i] = int.parse(hexStr.substring(i * 2, i * 2 + 2), radix: 16);
    }
    final msg = _decodeMessage(type, bytes);
    _printEvent({'event': 'decoded', 'data': _messageToJson(type, msg)});
    exit(0);
  }

  // Normal subscribe mode: <channel> <type> [--count N] [--timeout-ms MS] [--request-retained]
  final channel = args[0];
  final type = args[1];

  int count = 1;
  int timeoutMs = 5000;
  bool requestRetained = false;

  for (int i = 2; i < args.length; i++) {
    switch (args[i]) {
      case '--count':
        count = int.parse(args[++i]);
      case '--timeout-ms':
        timeoutMs = int.parse(args[++i]);
      case '--request-retained':
        requestRetained = true;
    }
  }

  final transport = await RaccoonTransport.create();
  int received = 0;
  final completer = Completer<void>();

  transport.subscribe(channel, (String ch, Uint8List data) {
    try {
      final msg = _decodeMessage(type, data);
      _printEvent({'event': 'received', 'data': _messageToJson(type, msg)});
      received++;
      if (received >= count && !completer.isCompleted) {
        completer.complete();
      }
    } catch (e) {
      stderr.writeln('Decode error: $e');
    }
  }, options: SubscribeOptions(requestRetained: requestRetained));

  _printEvent({'event': 'subscribed'});

  final timer = Timer(Duration(milliseconds: timeoutMs), () {
    if (!completer.isCompleted) {
      completer.completeError(
          'Timeout after ${timeoutMs}ms (received $received/$count)');
    }
  });

  try {
    await completer.future;
    timer.cancel();
    _printEvent({'event': 'done', 'count': received});
    transport.dispose();
    exit(0);
  } catch (e) {
    timer.cancel();
    stderr.writeln('$e');
    _printEvent({'event': 'timeout', 'count': received});
    transport.dispose();
    exit(1);
  }
}
