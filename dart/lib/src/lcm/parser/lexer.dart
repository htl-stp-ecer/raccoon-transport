import 'token.dart';

/// Lexer for LCM type definition files
class LcmLexer {
  final String source;
  final String filePath;

  int _position = 0;
  int _line = 1;
  int _column = 1;

  /// Accumulated doc comments for the next element
  final List<String> _docComments = [];

  static const _keywords = {
    'package': TokenType.package,
    'struct': TokenType.struct,
    'const': TokenType.const_,
  };

  LcmLexer(this.source, [this.filePath = '<unknown>']);

  /// Tokenize the source into a list of tokens
  List<Token> tokenize() {
    final tokens = <Token>[];

    while (!_isAtEnd) {
      _skipWhitespaceAndComments();
      if (_isAtEnd) break;

      final token = _scanToken();
      if (token != null) {
        tokens.add(token);
      }
    }

    tokens.add(Token(
      type: TokenType.eof,
      lexeme: '',
      line: _line,
      column: _column,
    ));

    return tokens;
  }

  bool get _isAtEnd => _position >= source.length;

  String get _current => _isAtEnd ? '' : source[_position];

  String _peek(int offset) {
    final pos = _position + offset;
    return pos < source.length ? source[pos] : '';
  }

  void _advance() {
    if (!_isAtEnd) {
      if (_current == '\n') {
        _line++;
        _column = 1;
      } else {
        _column++;
      }
      _position++;
    }
  }

  void _skipWhitespaceAndComments() {
    while (!_isAtEnd) {
      final c = _current;

      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        _advance();
      } else if (c == '/' && _peek(1) == '/') {
        _scanLineComment();
      } else if (c == '/' && _peek(1) == '*') {
        _scanBlockComment();
      } else {
        break;
      }
    }
  }

  void _scanLineComment() {
    // Check if it's a doc comment (///)
    final isDoc = _peek(2) == '/';

    // Skip the // or ///
    _advance();
    _advance();
    if (isDoc) _advance();

    final startPos = _position;

    // Read until end of line
    while (!_isAtEnd && _current != '\n') {
      _advance();
    }

    if (isDoc) {
      final content = source.substring(startPos, _position).trim();
      _docComments.add(content);
    }

    // Skip the newline
    if (!_isAtEnd) _advance();
  }

  void _scanBlockComment() {
    // Skip /*
    _advance();
    _advance();

    while (!_isAtEnd) {
      if (_current == '*' && _peek(1) == '/') {
        _advance();
        _advance();
        break;
      }
      _advance();
    }
  }

  Token? _scanToken() {
    final startLine = _line;
    final startColumn = _column;

    final c = _current;
    _advance();

    switch (c) {
      case ';':
        return _makeToken(TokenType.semicolon, ';', startLine, startColumn);
      case '{':
        return _makeToken(TokenType.openBrace, '{', startLine, startColumn);
      case '}':
        return _makeToken(TokenType.closeBrace, '}', startLine, startColumn);
      case '[':
        return _makeToken(TokenType.openBracket, '[', startLine, startColumn);
      case ']':
        return _makeToken(TokenType.closeBracket, ']', startLine, startColumn);
      case ',':
        return _makeToken(TokenType.comma, ',', startLine, startColumn);
      case '=':
        return _makeToken(TokenType.equals, '=', startLine, startColumn);
      case '.':
        return _makeToken(TokenType.dot, '.', startLine, startColumn);
      default:
        if (_isDigit(c) || (c == '-' && _isDigit(_current))) {
          return _scanNumber(c, startLine, startColumn);
        } else if (_isIdentifierStart(c)) {
          return _scanIdentifier(c, startLine, startColumn);
        } else {
          throw LexerException('Unexpected character: $c', startLine, startColumn);
        }
    }
  }

  Token _makeToken(TokenType type, String lexeme, int line, int column) {
    final doc = _docComments.isNotEmpty ? _docComments.join('\n') : null;
    _docComments.clear();
    return Token(
      type: type,
      lexeme: lexeme,
      line: line,
      column: column,
      docContent: doc,
    );
  }

  Token _scanNumber(String first, int startLine, int startColumn) {
    final buffer = StringBuffer(first);
    var isHex = false;
    var isFloat = false;

    // Check for hex prefix
    if (first == '0' && (_current == 'x' || _current == 'X')) {
      buffer.write(_current);
      _advance();
      isHex = true;

      // Read hex digits
      while (!_isAtEnd && _isHexDigit(_current)) {
        buffer.write(_current);
        _advance();
      }
    } else {
      // Read integer part
      while (!_isAtEnd && _isDigit(_current)) {
        buffer.write(_current);
        _advance();
      }

      // Check for decimal point
      if (_current == '.' && _isDigit(_peek(1))) {
        isFloat = true;
        buffer.write(_current);
        _advance();

        // Read fractional part
        while (!_isAtEnd && _isDigit(_current)) {
          buffer.write(_current);
          _advance();
        }
      }

      // Check for exponent
      if (_current == 'e' || _current == 'E') {
        isFloat = true;
        buffer.write(_current);
        _advance();

        if (_current == '+' || _current == '-') {
          buffer.write(_current);
          _advance();
        }

        while (!_isAtEnd && _isDigit(_current)) {
          buffer.write(_current);
          _advance();
        }
      }
    }

    final lexeme = buffer.toString();
    final type = isHex
        ? TokenType.hexLiteral
        : isFloat
            ? TokenType.floatLiteral
            : TokenType.integerLiteral;

    return _makeToken(type, lexeme, startLine, startColumn);
  }

  Token _scanIdentifier(String first, int startLine, int startColumn) {
    final buffer = StringBuffer(first);

    while (!_isAtEnd && _isIdentifierPart(_current)) {
      buffer.write(_current);
      _advance();
    }

    final lexeme = buffer.toString();
    final type = _keywords[lexeme] ?? TokenType.identifier;

    return _makeToken(type, lexeme, startLine, startColumn);
  }

  bool _isDigit(String c) => c.isNotEmpty && c.codeUnitAt(0) >= 48 && c.codeUnitAt(0) <= 57;

  bool _isHexDigit(String c) {
    if (c.isEmpty) return false;
    final code = c.codeUnitAt(0);
    return (code >= 48 && code <= 57) || // 0-9
        (code >= 65 && code <= 70) || // A-F
        (code >= 97 && code <= 102); // a-f
  }

  bool _isIdentifierStart(String c) {
    if (c.isEmpty) return false;
    final code = c.codeUnitAt(0);
    return (code >= 65 && code <= 90) || // A-Z
        (code >= 97 && code <= 122) || // a-z
        code == 95; // _
  }

  bool _isIdentifierPart(String c) {
    return _isIdentifierStart(c) || _isDigit(c);
  }
}
