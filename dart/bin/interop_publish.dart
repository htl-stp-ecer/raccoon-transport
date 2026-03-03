import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'package:raccoon_transport/raccoon_transport.dart';

void _printEvent(Map<String, dynamic> event) {
  stdout.writeln(jsonEncode(event));
}

LcmMessage _createMessage(String type, Map<String, dynamic> values) {
  switch (type) {
    case 'scalar_f_t':
      return ScalarFT(
        timestamp: values['timestamp'] as int,
        value: (values['value'] as num).toDouble(),
      );
    case 'scalar_i32_t':
      return ScalarI32T(
        timestamp: values['timestamp'] as int,
        value: values['value'] as int,
      );
    case 'vector3f_t':
      return Vector3fT(
        timestamp: values['timestamp'] as int,
        x: (values['x'] as num).toDouble(),
        y: (values['y'] as num).toDouble(),
        z: (values['z'] as num).toDouble(),
      );
    case 'string_t':
      return StringT(
        timestamp: values['timestamp'] as int,
        value: values['value'] as String,
      );
    default:
      throw ArgumentError('Unknown type: $type');
  }
}

Future<void> main(List<String> args) async {
  // --encode-hex <type> <json_values>: encode to hex without network
  if (args.isNotEmpty && args[0] == '--encode-hex') {
    final type = args[1];
    final values = jsonDecode(args[2]) as Map<String, dynamic>;
    final msg = _createMessage(type, values);
    final buf = LcmBuffer(65536);
    msg.encode(buf);
    final data = Uint8List.sublistView(buf.uint8List, 0, buf.position);
    final hex = data.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
    _printEvent({'event': 'encoded', 'hex': hex});
    exit(0);
  }

  final channel = args[0];
  final type = args[1];
  final values = jsonDecode(args[2]) as Map<String, dynamic>;

  int count = 1;
  int intervalMs = 0;
  bool retained = false;
  bool reliable = false;

  for (int i = 3; i < args.length; i++) {
    switch (args[i]) {
      case '--count':
        count = int.parse(args[++i]);
      case '--interval-ms':
        intervalMs = int.parse(args[++i]);
      case '--retained':
        retained = true;
      case '--reliable':
        reliable = true;
    }
  }

  final transport = await RaccoonTransport.create();

  try {
    final options = PublishOptions(retained: retained, reliable: reliable);
    for (int seq = 0; seq < count; seq++) {
      final msg = _createMessage(type, values);
      transport.publishMessage(channel, msg, options: options);
      _printEvent({'event': 'published', 'seq': seq});
      if (intervalMs > 0 && seq < count - 1) {
        await Future.delayed(Duration(milliseconds: intervalMs));
      }
    }

    if (retained || reliable) {
      _printEvent({'event': 'ready'});
      // Stay alive to serve retain requests / process reliable ACKs
      await stdin.drain();
    }
  } finally {
    transport.dispose();
  }
}
