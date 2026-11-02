/// Startup check of the transport contract. Semantics: contract/CHECK_SPEC.md.
///
/// Call [startupCheck] once when the app starts. The result is written to a
/// status file (not over the transport) and, when [ContractCheckResult.refuse]
/// is true, the app must not continue (the Bot UI shows a popup).
library;

import 'dart:convert';
import 'dart:io';

import 'contract.dart';

// The own hash is derived from the marker string, not from the separate hash
// constant. That keeps the full "RACCOON_CONTRACT_V1=<hash>" literal reachable
// from the startup check, so it survives tree shaking in the AOT build and the
// image build can find it in the compiled app.
String _ownHashFromMarker() => raccoonContractMarker.substring(raccoonContractMarker.indexOf('=') + 1);

/// Reserved exit code of a component that refuses to start.
const int contractExitCode = 78;

const String _defaultBundleFile = '/etc/raccoon-bundle.json';
const String _defaultDeviceMarker = '/etc/raccoon-image';
const String _defaultStatusDir = '/run/raccoon';

const String contractOk = 'ok';
const String contractMismatch = 'mismatch';
const String contractDevWarning = 'dev-warnung';
const String contractBundleError = 'bundle-fehler';
const String contractVersionMismatch = 'version-mismatch';
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
    String? bundleKey,
  }) : bundleKey = bundleKey ?? component;

  final String component;
  final String componentVersion;
  final String ownHash;
  final String ownTransportVersion;
  final String bundleFile;

  /// This component's entry in the bundle's "components" map.
  final String bundleKey;
  String? expectedComponentVersion;
  String? expectedHash;
  String? expectedTransportVersion;
  String? bundle;
  String result = contractSkipped;
  String? reason;
  String message = '';

  /// The component must not start.
  bool get refuse =>
      result == contractMismatch || result == contractBundleError || result == contractVersionMismatch;
  bool get devMode => result == contractDevWarning;
}

String _name(String? value) => (value == null || value.isEmpty) ? 'unknown' : value;

/// `v173` and `173` are the same version; nothing else is normalised.
String _normalizeVersion(String? value) {
  final t = (value ?? '').trim();
  return (t.isNotEmpty && (t[0] == 'v' || t[0] == 'V')) ? t.substring(1) : t;
}

String _message(ContractCheckResult r) {
  switch (r.result) {
    case contractOk:
      return 'contract ok: ${r.ownHash}';
    case contractSkipped:
      return 'not running on a raccoon device; contract check skipped';
    case contractVersionMismatch:
      return 'component version mismatch: ${r.component} is ${_name(r.componentVersion)} but bundle '
          '${_name(r.bundle)} expects ${r.expectedComponentVersion}; '
          'update or reflash so all components match';
    case contractDevWarning:
      if (r.reason == 'version-omitted') {
        return "component version check omitted ('${r.bundleKey}' is 'dev' in the bundle): "
            '${r.component} ${_name(r.componentVersion)} is not compared; mismatches are not enforced';
      }
      return "transport contract check is in dev mode (bundle field is 'dev'): "
          '${r.component} uses contract ${r.ownHash} (transport ${_name(r.ownTransportVersion)}); '
          'mismatches are not enforced';
    case contractMismatch:
      return 'transport contract mismatch: ${r.component} ${_name(r.componentVersion)} uses contract '
          '${r.ownHash} (transport ${_name(r.ownTransportVersion)}), bundle ${_name(r.bundle)} expects '
          '${r.expectedHash} (transport ${_name(r.expectedTransportVersion)}); '
          'update or reflash so all components match';
  }
  if (r.reason == 'version-field-missing' || r.reason == 'version-field-empty') {
    final what = r.reason == 'version-field-missing' ? 'no version' : 'an empty version';
    return 'bundle check failed: bundle file has $what for ${r.bundleKey}: ${r.bundleFile}';
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
  String? bundleKey,
}) {
  final bundlePath = _pick(bundleFile, 'RACCOON_BUNDLE_FILE', _defaultBundleFile);
  final markerPath = _pick(deviceMarker, 'RACCOON_DEVICE_MARKER', _defaultDeviceMarker);
  final forced = force ?? Platform.environment['RACCOON_CONTRACT_CHECK'] == 'force';
  final r = ContractCheckResult(
    component: component,
    componentVersion: componentVersion,
    ownHash: ownHash ?? _ownHashFromMarker(),
    ownTransportVersion: ownVersion ?? transportVersion,
    bundleFile: bundlePath,
    bundleKey: bundleKey,
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
  final contractDev = trimmed == 'dev';
  if (!contractDev && trimmed != r.ownHash) return done(contractMismatch);

  // The component's own version against the bundle's entry for it. Independent of the
  // contract: a dev contract does not excuse a wrong version, and 'dev' in the
  // component's own field omits only this comparison.
  final entry = components is Map<String, dynamic> ? components[r.bundleKey] : null;
  if (entry is! String) return done(contractBundleError, 'version-field-missing');
  final expectedVersion = entry.trim();
  if (expectedVersion.isEmpty) return done(contractBundleError, 'version-field-empty');
  r.expectedComponentVersion = expectedVersion;
  final versionOmitted = expectedVersion == 'dev';
  if (!versionOmitted && _normalizeVersion(expectedVersion) != _normalizeVersion(componentVersion)) {
    return done(contractVersionMismatch);
  }
  if (contractDev) return done(contractDevWarning);
  if (versionOmitted) return done(contractDevWarning, 'version-omitted');
  return done(contractOk);
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
    'expected_component_version': r.expectedComponentVersion,
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
  String? bundleKey,
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
    bundleKey: bundleKey,
  );
  if (r.result != contractSkipped) writeContractStatus(r, persistent: persistent, statusDir: statusDir);
  final level = r.refuse ? 'error' : (r.devMode ? 'warning' : 'info');
  stderr.writeln('$level: $component: ${r.message}');
  return r;
}
