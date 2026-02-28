/// Primitive types in LCM
const primitiveTypes = {
  'int8_t',
  'int16_t',
  'int32_t',
  'int64_t',
  'float',
  'double',
  'string',
  'boolean',
  'byte',
};

/// A complete LCM file
class LcmFile {
  final String path;
  final String? package;
  final List<StructDecl> structs;
  final String? fileComment;

  LcmFile({
    required this.path,
    this.package,
    required this.structs,
    this.fileComment,
  });

  @override
  String toString() => 'LcmFile(package: $package, structs: ${structs.length})';
}

/// A struct definition
class StructDecl {
  final String name;
  final String? package;
  final List<MemberDecl> members;
  final List<ConstantDecl> constants;
  final String? docComment;
  final int line;

  StructDecl({
    required this.name,
    this.package,
    required this.members,
    required this.constants,
    this.docComment,
    required this.line,
  });

  /// Get the full qualified name
  String get fullName => package != null ? '$package.$name' : name;

  @override
  String toString() => 'StructDecl($fullName, members: ${members.length}, constants: ${constants.length})';
}

/// A member field declaration
class MemberDecl {
  final TypeRef type;
  final String name;
  final List<ArrayDimension> dimensions;
  final String? docComment;
  final int line;

  MemberDecl({
    required this.type,
    required this.name,
    required this.dimensions,
    this.docComment,
    required this.line,
  });

  /// Whether this member is an array
  bool get isArray => dimensions.isNotEmpty;

  /// Whether this member is a fixed-size array
  bool get isFixedArray => dimensions.isNotEmpty && dimensions.every((d) => d.isConstant);

  /// Whether this member is a variable-size array
  bool get isVariableArray => dimensions.isNotEmpty && dimensions.any((d) => !d.isConstant);

  @override
  String toString() => 'MemberDecl(${type.fullName} $name${dimensions.map((d) => '[$d]').join()})';
}

/// A constant declaration
class ConstantDecl {
  final String type;
  final String name;
  final String valueString;
  final dynamic value;
  final String? docComment;
  final int line;

  ConstantDecl({
    required this.type,
    required this.name,
    required this.valueString,
    required this.value,
    this.docComment,
    required this.line,
  });

  @override
  String toString() => 'ConstantDecl($type $name = $valueString)';
}

/// An array dimension
class ArrayDimension {
  /// Whether this is a constant dimension (fixed size)
  final bool isConstant;

  /// The size expression (either a number string or variable name)
  final String size;

  /// The resolved numeric value (if constant)
  final int? value;

  ArrayDimension({
    required this.isConstant,
    required this.size,
    this.value,
  });

  @override
  String toString() => isConstant ? size : 'var:$size';
}

/// A type reference
class TypeRef {
  /// The full name including package (e.g., "lcmtest.primitives_t")
  final String fullName;

  /// The package name (null for primitives or same-package types)
  final String? package;

  /// The short name without package
  final String shortName;

  TypeRef({
    required this.fullName,
    this.package,
    required this.shortName,
  });

  /// Whether this is a primitive type
  bool get isPrimitive => primitiveTypes.contains(fullName);

  /// Create a TypeRef from a full name
  factory TypeRef.parse(String name, String? currentPackage) {
    if (primitiveTypes.contains(name)) {
      return TypeRef(fullName: name, shortName: name);
    }

    // Check if it's a qualified name
    final dotIndex = name.lastIndexOf('.');
    if (dotIndex != -1) {
      final pkg = name.substring(0, dotIndex);
      final short = name.substring(dotIndex + 1);
      return TypeRef(fullName: name, package: pkg, shortName: short);
    }

    // Unqualified non-primitive type - use current package
    if (currentPackage != null) {
      return TypeRef(
        fullName: '$currentPackage.$name',
        package: currentPackage,
        shortName: name,
      );
    }

    return TypeRef(fullName: name, shortName: name);
  }

  @override
  String toString() => fullName;
}

/// Exception thrown during parsing
class ParseException implements Exception {
  final String message;
  final int line;
  final int column;

  ParseException(this.message, this.line, this.column);

  @override
  String toString() => 'ParseException at $line:$column: $message';
}
