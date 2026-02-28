import 'token.dart';
import 'ast.dart';

/// Parser for LCM type definition files
class LcmParser {
  final List<Token> tokens;
  final String filePath;
  int _current = 0;

  /// Current package context
  String? _currentPackage;

  LcmParser(this.tokens, [this.filePath = '<unknown>']);

  /// Parse tokens into an AST
  LcmFile parse() {
    String? fileComment;
    String? package;
    final structs = <StructDecl>[];

    // Check for file-level doc comment
    if (_check(TokenType.package) && _peek().docContent != null) {
      fileComment = _peek().docContent;
    }

    // Parse optional package declaration
    if (_check(TokenType.package)) {
      package = _parsePackage();
      _currentPackage = package;
    }

    // Parse struct definitions
    while (!_isAtEnd) {
      structs.add(_parseStruct());
    }

    return LcmFile(
      path: filePath,
      package: package,
      structs: structs,
      fileComment: fileComment,
    );
  }

  // Helper methods

  bool get _isAtEnd => _peek().type == TokenType.eof;

  Token _peek() => tokens[_current];

  Token _previous() => tokens[_current - 1];

  bool _check(TokenType type) => !_isAtEnd && _peek().type == type;

  bool _match(TokenType type) {
    if (_check(type)) {
      _advance();
      return true;
    }
    return false;
  }

  Token _advance() {
    if (!_isAtEnd) _current++;
    return _previous();
  }

  Token _consume(TokenType type, String message) {
    if (_check(type)) return _advance();
    throw ParseException(message, _peek().line, _peek().column);
  }

  // Parse methods

  String _parsePackage() {
    _consume(TokenType.package, 'Expected "package"');

    final parts = <String>[];
    parts.add(_consume(TokenType.identifier, 'Expected package name').lexeme);

    while (_match(TokenType.dot)) {
      parts.add(_consume(TokenType.identifier, 'Expected package name part').lexeme);
    }

    _consume(TokenType.semicolon, 'Expected ";" after package declaration');

    return parts.join('.');
  }

  StructDecl _parseStruct() {
    final docComment = _peek().docContent;
    final line = _peek().line;

    _consume(TokenType.struct, 'Expected "struct"');
    final name = _consume(TokenType.identifier, 'Expected struct name').lexeme;
    _consume(TokenType.openBrace, 'Expected "{"');

    final members = <MemberDecl>[];
    final constants = <ConstantDecl>[];

    while (!_check(TokenType.closeBrace) && !_isAtEnd) {
      if (_check(TokenType.const_)) {
        constants.addAll(_parseConstants());
      } else {
        members.add(_parseMember(constants, members));
      }
    }

    _consume(TokenType.closeBrace, 'Expected "}"');

    return StructDecl(
      name: name,
      package: _currentPackage,
      members: members,
      constants: constants,
      docComment: docComment,
      line: line,
    );
  }

  List<ConstantDecl> _parseConstants() {
    final docComment = _peek().docContent;
    final line = _peek().line;

    _consume(TokenType.const_, 'Expected "const"');
    final type = _consume(TokenType.identifier, 'Expected constant type').lexeme;

    final constants = <ConstantDecl>[];

    do {
      final name = _consume(TokenType.identifier, 'Expected constant name').lexeme;
      _consume(TokenType.equals, 'Expected "="');

      final valueToken = _advance();
      final valueString = valueToken.lexeme;
      final value = _parseConstantValue(valueToken);

      constants.add(ConstantDecl(
        type: type,
        name: name,
        valueString: valueString,
        value: value,
        docComment: constants.isEmpty ? docComment : null,
        line: line,
      ));
    } while (_match(TokenType.comma));

    _consume(TokenType.semicolon, 'Expected ";" after constant declaration');

    return constants;
  }

  dynamic _parseConstantValue(Token token) {
    switch (token.type) {
      case TokenType.integerLiteral:
        return int.parse(token.lexeme);
      case TokenType.hexLiteral:
        return int.parse(token.lexeme);
      case TokenType.floatLiteral:
        return double.parse(token.lexeme);
      default:
        throw ParseException(
          'Expected constant value, got ${token.type}',
          token.line,
          token.column,
        );
    }
  }

  MemberDecl _parseMember(List<ConstantDecl> constants, List<MemberDecl> members) {
    final docComment = _peek().docContent;
    final line = _peek().line;

    final type = _parseTypeRef();
    final name = _consume(TokenType.identifier, 'Expected member name').lexeme;
    final dimensions = _parseArrayDimensions(constants, members);

    _consume(TokenType.semicolon, 'Expected ";" after member declaration');

    return MemberDecl(
      type: type,
      name: name,
      dimensions: dimensions,
      docComment: docComment,
      line: line,
    );
  }

  TypeRef _parseTypeRef() {
    final parts = <String>[];
    parts.add(_consume(TokenType.identifier, 'Expected type name').lexeme);

    while (_match(TokenType.dot)) {
      parts.add(_consume(TokenType.identifier, 'Expected type name part').lexeme);
    }

    final fullName = parts.join('.');
    return TypeRef.parse(fullName, _currentPackage);
  }

  List<ArrayDimension> _parseArrayDimensions(
    List<ConstantDecl> constants,
    List<MemberDecl> members,
  ) {
    final dimensions = <ArrayDimension>[];

    while (_match(TokenType.openBracket)) {
      final sizeToken = _advance();
      final size = sizeToken.lexeme;

      bool isConstant;
      int? value;

      if (sizeToken.type == TokenType.integerLiteral) {
        // Literal number - constant dimension
        isConstant = true;
        value = int.parse(size);
      } else if (sizeToken.type == TokenType.identifier) {
        // Check if it references a constant
        final constant = constants.cast<ConstantDecl?>().firstWhere(
          (c) => c!.name == size,
          orElse: () => null,
        );

        if (constant != null) {
          isConstant = true;
          value = constant.value as int;
        } else {
          // Check if it references another member (variable-length array)
          final member = members.cast<MemberDecl?>().firstWhere(
            (m) => m!.name == size,
            orElse: () => null,
          );

          if (member != null) {
            isConstant = false;
          } else {
            // Assume it's a forward reference to a constant or unknown
            // For fingerprint calculation, we treat unknown identifiers as constants
            isConstant = true;
          }
        }
      } else {
        throw ParseException(
          'Expected array size, got ${sizeToken.type}',
          sizeToken.line,
          sizeToken.column,
        );
      }

      _consume(TokenType.closeBracket, 'Expected "]"');

      dimensions.add(ArrayDimension(
        isConstant: isConstant,
        size: size,
        value: value,
      ));
    }

    return dimensions;
  }
}
