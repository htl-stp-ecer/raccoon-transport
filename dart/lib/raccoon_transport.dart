/// Shared LCM transport library with channel definitions and optional reliable delivery
library raccoon_transport;

// Core LCM functionality (migrated from lcm_dart)
export 'src/lcm/lcm_buffer.dart';
export 'src/lcm/lcm.dart';

// Parser (for programmatic use)
export 'src/lcm/parser/token.dart';
export 'src/lcm/parser/lexer.dart';
export 'src/lcm/parser/ast.dart';
export 'src/lcm/parser/parser.dart';

// Generator (for programmatic use)
export 'src/lcm/generator/fingerprint.dart';
export 'src/lcm/generator/type_mapper.dart';
export 'src/lcm/generator/dart_generator.dart';

// Transport
export 'src/channels.dart';
export 'src/transport.dart';
