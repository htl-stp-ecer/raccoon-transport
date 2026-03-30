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

// Generated message types
export 'messages/types/orientation_matrix_t.g.dart';
export 'messages/types/quaternion_t.g.dart';
export 'messages/types/scalar_f_t.g.dart';
export 'messages/types/scalar_i32_t.g.dart';
export 'messages/types/scalar_i8_t.g.dart';
export 'messages/types/screen_render_answer_t.g.dart';
export 'messages/types/screen_render_t.g.dart';
export 'messages/types/string_t.g.dart';
export 'messages/types/vector3f_t.g.dart';
export 'messages/types/cam_blob_t.g.dart';
export 'messages/types/cam_detections_t.g.dart';
export 'messages/types/cam_frame_t.g.dart';
export 'messages/types/cam_stream_ctl_t.g.dart';
export 'messages/types/cam_config_t.g.dart';
