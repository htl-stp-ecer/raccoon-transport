/// Startup check of the transport contract. Semantics: contract/CHECK_SPEC.md.
///
/// Call [startupCheck] once when the app starts. The result is written to a
/// status file (not over the transport) and, when [ContractCheckResult.refuse]
/// is true, the app must not continue (the Bot UI shows a popup).
library;

import 'dart:convert';
import 'dart:io';

import 'contract.dart';

/// Reserved exit code of a component that refuses to start.
const int contractExitCode = 78;

const String _defaultBundleFile = '/etc/raccoon-bundle.json';
const String _defaultDeviceMarker = '/etc/raccoon-image';
const String _defaultStatusDir = '/run/raccoon';

const String contractOk = 'ok';
const String contractMismatch = 'mismatch';
const String contractDevWarning = 'dev-warnung';
const String contractBundleError = 'bundle-fehler';
const String contractSkipped = 'skipped';

const Map<String, String> _reasonText = {
  'file-missing': 'bundle file is missing',
  'file-unreadable': 'bundle file is unreadable',
  'field-missing': 'bundle file has no contract_hash field',
  'field-empty': 'bundle file has an empty contract_hash',
};

class ContractCheckResult {
  ContractCheckResult({
    required this.component,
    required this.componentVersion,
    required this.ownHash,
    required this.ownTransportVersion,
    required this.bundleFile,
  });

  final String component;
  final String componentVersion;
  final String ownHash;
  final String ownTransportVersion;
  final String bundleFile;
  String? expectedHash;
  String? expectedTransportVersion;
  String? bundle;
  String result = contractSkipped;
  String? reason;
  String message = '';

  /// The component must not start.
  bool get refuse => result == contractMismatch || result == contractBundleError;
  bool get devMode => result == contractDevWarning;
}

String _name(String? value) => (value == null || value.isEmpty) ? 'unknown' : value;

String _message(ContractCheckResult r) {
  switch (r.result) {
    case contractOk:
      return 'contract ok: ${r.ownHash}';
    case contractSkipped:
      return 'not running on a raccoon device; contract check skipped';
    case contractDevWarning:
      return "transport contract check is in dev mode (bundle field is 'dev'): "
          '${r.component} uses contract ${r.ownHash} (transport ${_name(r.ownTransportVersion)}); '
          'mismatches are not enforced';
    case contractMismatch:
      return 'transport contract mismatch: ${r.component} ${_name(r.componentVersion)} uses contract '
          '${r.ownHash} (transport ${_name(r.ownTransportVersion)}), bundle ${_name(r.bundle)} expects '
          '${r.expectedHash} (transport ${_name(r.expectedTransportVersion)}); '
          'update or reflash so all components match';
  }
  return 'transport contract check failed: '
      '${_reasonText[r.reason ?? 'file-unreadable']}: ${r.bundleFile}';
}

String _pick(String? given, String env, String fallback) {
  if (given != null && given.isNotEmpty) return given;
  final value = Platform.environment[env];
  if (value != null && value.isNotEmpty) return value;
  return fallback;
}

/// The decision of the spec. Reads one file, writes nothing.
ContractCheckResult checkContract(
  String component,
  String componentVersion, {
  String? ownHash,
  String? ownVersion,
  String? bundleFile,
  String? deviceMarker,
  bool? force,
  String transportVersion = 'dev',
}) {
  final bundlePath = _pick(bundleFile, 'RACCOON_BUNDLE_FILE', _defaultBundleFile);
  final markerPath = _pick(deviceMarker, 'RACCOON_DEVICE_MARKER', _defaultDeviceMarker);
  final forced = force ?? Platform.environment['RACCOON_CONTRACT_CHECK'] == 'force';
  final r = ContractCheckResult(
    component: component,
    componentVersion: componentVersion,
    ownHash: ownHash ?? raccoonContractHash,
    ownTransportVersion: ownVersion ?? transportVersion,
    bundleFile: bundlePath,
  );

  ContractCheckResult done(String outcome, [String? reason]) {
    r.result = outcome;
    r.reason = reason;
    r.message = _message(r);
    return r;
  }

  if (!forced && !Directory(markerPath).existsSync()) return done(contractSkipped);

  final file = File(bundlePath);
  if (!file.existsSync()) {
    // A directory or other non-file at that path is unreadable, not missing.
    final type = FileSystemEntity.typeSync(bundlePath);
    return done(contractBundleError, type == FileSystemEntityType.notFound ? 'file-missing' : 'file-unreadable');
  }
  String text;
  try {
    text = file.readAsStringSync();
  } on FileSystemException {
    return done(contractBundleError, 'file-unreadable');
  } on FormatException {
    return done(contractBundleError, 'file-unreadable');
  }
  Object? document;
  try {
    document = jsonDecode(text);
  } on FormatException {
    return done(contractBundleError, 'file-unreadable');
  }
  if (document is! Map<String, dynamic>) return done(contractBundleError, 'file-unreadable');

  final bundle = document['bundle'];
  if (bundle is String) r.bundle = bundle;
  final components = document['components'];
  if (components is Map<String, dynamic> && components['raccoon-transport'] is String) {
    r.expectedTransportVersion = components['raccoon-transport'] as String;
  }

  final expected = document['contract_hash'];
  if (expected is! String) return done(contractBundleError, 'field-missing');
  final trimmed = expected.trim();
  if (trimmed.isEmpty) return done(contractBundleError, 'field-empty');
  r.expectedHash = trimmed;
  if (trimmed == 'dev') return done(contractDevWarning);
  return done(trimmed == r.ownHash ? contractOk : contractMismatch);
}

int? _ownStartTime() {
  try {
    final stat = File('/proc/self/stat').readAsStringSync();
    final fields = stat.substring(stat.lastIndexOf(')') + 2).split(' ');
    return int.parse(fields[19]);
  } catch (_) {
    return null;
  }
}

/// Writes `<statusDir>/<component>.json` atomically. Returns false on failure
/// (the component must keep running).
bool writeContractStatus(ContractCheckResult r, {required bool persistent, String? statusDir}) {
  final dir = Directory(_pick(statusDir, 'RACCOON_STATUS_DIR', _defaultStatusDir));
  final document = <String, Object?>{
    'component': r.component,
    'component_version': r.componentVersion,
    'own_hash': r.ownHash,
    'own_transport_version': r.ownTransportVersion,
    'expected_hash': r.expectedHash,
    'expected_transport_version': r.expectedTransportVersion,
    'bundle': r.bundle,
    'result': r.result,
    'reason': r.reason,
    'message': r.message,
    'timestamp': DateTime.now().toUtc().toIso8601String().split('.').first + 'Z',
    'pid': pid,
    'pid_starttime': _ownStartTime(),
    'persistent': persistent,
  };
  try {
    dir.createSync(recursive: true);
    final tmp = File('${dir.path}/.${r.component}.$pid.tmp');
    tmp.writeAsStringSync('${const JsonEncoder.withIndent('  ').convert(document)}\n', flush: true);
    tmp.renameSync('${dir.path}/${r.component}.json');
    return true;
  } on FileSystemException catch (error) {
    stderr.writeln('warning: could not write contract status file in ${dir.path}: ${error.message}');
    return false;
  }
}

/// Check, record, log. The caller decides what to do with [ContractCheckResult.refuse].
ContractCheckResult startupCheck(
  String component,
  String componentVersion, {
  required bool persistent,
  String? statusDir,
  String? ownHash,
  String? ownVersion,
  String? bundleFile,
  String? deviceMarker,
  bool? force,
  String transportVersion = 'dev',
}) {
  final r = checkContract(
    component,
    componentVersion,
    ownHash: ownHash,
    ownVersion: ownVersion,
    bundleFile: bundleFile,
    deviceMarker: deviceMarker,
    force: force,
    transportVersion: transportVersion,
  );
  if (r.result != contractSkipped) writeContractStatus(r, persistent: persistent, statusDir: statusDir);
  final level = r.refuse ? 'error' : (r.devMode ? 'warning' : 'info');
  stderr.writeln('$level: $component: ${r.message}');
  return r;
}
