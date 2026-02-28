import '../parser/ast.dart';

/// Calculator for LCM type fingerprints
///
/// The fingerprint is a 64-bit hash that uniquely identifies a message type.
/// It's used for runtime type checking when decoding messages.
///
/// Algorithm: https://lcm-proj.github.io/lcm/content/lcm-type-ref.html#fingerprint-computation
class FingerprintCalculator {
  /// Map of struct full names to their declarations
  final Map<String, StructDecl> _structsByName = {};

  /// Register all structs from an LCM file for nested type lookup
  void registerStructs(LcmFile file) {
    for (final struct in file.structs) {
      _structsByName[struct.fullName] = struct;
    }
  }

  /// Register a single struct for nested type lookup
  void registerStruct(StructDecl struct) {
    _structsByName[struct.fullName] = struct;
  }

  /// Compute the fingerprint for a struct
  ///
  /// Returns the final fingerprint value including recursive hashes of nested types
  int computeFingerprint(StructDecl struct) {
    return _computeFingerprintRecursive(struct, {});
  }

  /// Recursively compute fingerprint, tracking visited types to handle circular refs
  int _computeFingerprintRecursive(StructDecl struct, Set<String> visited) {
    final baseHash = computeStructHash(struct);

    // Check for circular reference - return 0 if already visited
    if (visited.contains(struct.fullName)) {
      return 0;
    }

    // Add current struct to visited set
    final newVisited = {...visited, struct.fullName};

    // Start with the base hash
    int fingerprint = baseHash;

    // Add fingerprints of all non-primitive member types
    for (final member in struct.members) {
      if (!member.type.isPrimitive) {
        final nestedStruct = _findStruct(member.type.fullName);
        if (nestedStruct != null) {
          fingerprint += _computeFingerprintRecursive(nestedStruct, newVisited);
        }
      }
    }

    // Apply the LCM fingerprint transformation: (hash << 1) + (hash >> 63)
    return _transformToFingerprint(fingerprint);
  }

  /// Find a struct by its full name
  StructDecl? _findStruct(String fullName) {
    return _structsByName[fullName];
  }

  /// Compute the base hash for a struct (before fingerprint transformation)
  int computeStructHash(StructDecl struct) {
    int v = 0x12345678;

    for (final member in struct.members) {
      // Hash the member name
      v = _hashStringUpdate(v, member.name);

      // If the member is a primitive type, include the type signature
      // Do not include compound types - their contents will be included instead
      if (member.type.isPrimitive) {
        v = _hashStringUpdate(v, member.type.fullName);
      }

      // Hash the dimensionality information
      final ndim = member.dimensions.length;
      v = _hashUpdate(v, ndim);

      for (final dim in member.dimensions) {
        // Hash the dimension mode: 0 for constant, 1 for variable
        v = _hashUpdate(v, dim.isConstant ? 0 : 1);
        // Hash the size string
        v = _hashStringUpdate(v, dim.size);
      }
    }

    return v;
  }

  /// Transform a base hash to a fingerprint
  ///
  /// Uses the formula: (hash << 1) + (hash >> 63)
  /// with unsigned 64-bit arithmetic
  int _transformToFingerprint(int hash) {
    // Use >>> for logical (unsigned) right shift
    return ((hash << 1) + (hash >>> 63)).toSigned(64);
  }

  /// Update hash with a single byte value
  ///
  /// Formula: ((v << 8) ^ (v >> 55)) + c
  int _hashUpdate(int v, int c) {
    // Use arithmetic right shift (>>) to match C's int64_t behavior
    return ((v << 8) ^ (v >> 55)) + c;
  }

  /// Update hash with a string
  ///
  /// First hashes the length, then each character
  int _hashStringUpdate(int v, String s) {
    v = _hashUpdate(v, s.length);
    for (final c in s.codeUnits) {
      v = _hashUpdate(v, c);
    }
    return v;
  }
}
