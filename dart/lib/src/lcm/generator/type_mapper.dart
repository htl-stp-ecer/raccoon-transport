/// Maps LCM types to Dart types and buffer methods
class TypeMapper {
  /// Map an LCM type to its Dart type
  String mapType(String lcmType) {
    return switch (lcmType) {
      'int8_t' || 'int16_t' || 'int32_t' || 'int64_t' || 'byte' => 'int',
      'float' || 'double' => 'double',
      'string' => 'String',
      'boolean' => 'bool',
      _ => toPascalCase(_shortName(lcmType)),
    };
  }

  /// Map an LCM type to its Dart list type for arrays
  String mapArrayType(String lcmType) {
    final dartType = mapType(lcmType);
    return 'List<$dartType>';
  }

  /// Get the buffer method name for encoding a type
  String getEncodeMethod(String lcmType) {
    return switch (lcmType) {
      'int8_t' => 'putInt8',
      'int16_t' => 'putInt16',
      'int32_t' => 'putInt32',
      'int64_t' => 'putInt64',
      'byte' => 'putUint8',
      'float' => 'putFloat32',
      'double' => 'putFloat64',
      'boolean' => 'putUint8',
      'string' => throw ArgumentError('string needs special handling'),
      _ => throw ArgumentError('Cannot encode non-primitive type: $lcmType'),
    };
  }

  /// Get the buffer method name for decoding a type
  String getDecodeMethod(String lcmType) {
    return switch (lcmType) {
      'int8_t' => 'getInt8',
      'int16_t' => 'getInt16',
      'int32_t' => 'getInt32',
      'int64_t' => 'getInt64',
      'byte' => 'getUint8',
      'float' => 'getFloat32',
      'double' => 'getFloat64',
      'boolean' => 'getUint8',
      'string' => throw ArgumentError('string needs special handling'),
      _ => throw ArgumentError('Cannot decode non-primitive type: $lcmType'),
    };
  }

  /// Check if a type is a numeric primitive (can use standard buffer methods)
  bool isNumericPrimitive(String lcmType) {
    return switch (lcmType) {
      'int8_t' ||
      'int16_t' ||
      'int32_t' ||
      'int64_t' ||
      'byte' ||
      'float' ||
      'double' =>
        true,
      _ => false,
    };
  }

  /// Check if a type needs special encoding/decoding (string, boolean)
  bool needsSpecialHandling(String lcmType) {
    return lcmType == 'string' || lcmType == 'boolean';
  }

  /// Convert snake_case or lowercase name to PascalCase
  String toPascalCase(String name) {
    final parts = name.split('_');
    return parts.map((part) {
      if (part.isEmpty) return '';
      return part[0].toUpperCase() + part.substring(1).toLowerCase();
    }).join();
  }

  /// Convert PascalCase to snake_case
  String toSnakeCase(String name) {
    final result = StringBuffer();
    for (int i = 0; i < name.length; i++) {
      final char = name[i];
      if (i > 0 && char.toUpperCase() == char && char.toLowerCase() != char) {
        result.write('_');
      }
      result.write(char.toLowerCase());
    }
    return result.toString();
  }

  /// Get the short name from a fully qualified type name
  String _shortName(String fullName) {
    final dotIndex = fullName.lastIndexOf('.');
    return dotIndex >= 0 ? fullName.substring(dotIndex + 1) : fullName;
  }

  /// Get default value for a Dart type
  String getDefaultValue(String lcmType, bool isArray) {
    if (isArray) return '[]';
    return switch (lcmType) {
      'int8_t' || 'int16_t' || 'int32_t' || 'int64_t' || 'byte' => '0',
      'float' || 'double' => '0.0',
      'string' => "''",
      'boolean' => 'false',
      _ => 'null', // Custom types need special handling
    };
  }
}
