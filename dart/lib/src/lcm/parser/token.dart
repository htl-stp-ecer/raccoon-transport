/// Token types for LCM lexer
enum TokenType {
  // Keywords
  package,
  struct,
  const_,

  // Punctuation
  semicolon, // ;
  openBrace, // {
  closeBrace, // }
  openBracket, // [
  closeBracket, // ]
  comma, // ,
  equals, // =
  dot, // .

  // Literals
  identifier,
  integerLiteral,
  hexLiteral,
  floatLiteral,
  stringLiteral,

  // Special
  eof,
}

/// A token produced by the lexer
class Token {
  final TokenType type;
  final String lexeme;
  final int line;
  final int column;

  /// For doc comments, the content without the /// prefix
  final String? docContent;

  const Token({
    required this.type,
    required this.lexeme,
    required this.line,
    required this.column,
    this.docContent,
  });

  @override
  String toString() => 'Token($type, "$lexeme", $line:$column)';
}

/// Exception thrown during lexing
class LexerException implements Exception {
  final String message;
  final int line;
  final int column;

  LexerException(this.message, this.line, this.column);

  @override
  String toString() => 'LexerException at $line:$column: $message';
}
