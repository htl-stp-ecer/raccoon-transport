// Runs the shared vectors of ../contract/check_vectors.json through the Dart check.
import 'dart:convert';
import 'dart:io';

import 'package:raccoon_transport/raccoon_transport.dart';
import 'package:test/test.dart';

void main() {
  final vectors = jsonDecode(File('../contract/check_vectors.json').readAsStringSync()) as Map<String, dynamic>;
  final defaults = vectors['defaults'] as Map<String, dynamic>;

  group('shared vectors', () {
    for (final raw in vectors['cases'] as List<dynamic>) {
      final c = raw as Map<String, dynamic>;
      test(c['name'] as String, () {
        final tmp = Directory.systemTemp.createTempSync('raccoon_check_');
        addTearDown(() => tmp.deleteSync(recursive: true));
        final marker = Directory('${tmp.path}/image');
        if (c['on_device'] == true) marker.createSync();
        final bundle = File('${tmp.path}/bundle.json');
        if (c['bundle'] != null) bundle.writeAsStringSync(c['bundle'] as String);

        final r = checkContract(
          defaults['component'] as String,
          defaults['component_version'] as String,
          ownHash: defaults['own_hash'] as String,
          ownVersion: defaults['own_version'] as String,
          bundleFile: bundle.path,
          deviceMarker: marker.path,
          force: c['force'] == true,
        );
        expect(r.result, c['result']);
        expect(r.reason, c['reason']);
        expect(r.refuse, c['refuse']);
        expect(r.expectedHash, c['expected_hash']);
        expect(r.message.replaceAll(bundle.path, defaults['bundle_file'] as String), c['message']);
      });
    }
  });

  test('status file is written atomically with the result', () {
    final tmp = Directory.systemTemp.createTempSync('raccoon_status_');
    addTearDown(() => tmp.deleteSync(recursive: true));
    File('${tmp.path}/bundle.json').writeAsStringSync('{"bundle":"b","contract_hash":"bbbb"}');
    final r = startupCheck(
      'botui',
      'v73',
      persistent: true,
      statusDir: '${tmp.path}/run',
      ownHash: 'aaaa',
      ownVersion: 'v9',
      bundleFile: '${tmp.path}/bundle.json',
      deviceMarker: tmp.path,
    );
    expect(r.refuse, isTrue);
    final written = jsonDecode(File('${tmp.path}/run/botui.json').readAsStringSync()) as Map<String, dynamic>;
    expect(written['result'], 'mismatch');
    expect(written['persistent'], true);
    expect(written['pid'], pid);
    expect(contractExitCode, 78);
  });
}
