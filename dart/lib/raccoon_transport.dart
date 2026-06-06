/// Shared LCM transport library with channel definitions and optional reliable delivery
library raccoon_transport;

// Core LCM functionality (migrated from lcm_dart). The standalone LCM
// parser/generator + builder were removed in cc7d9e7 when message types
// became hand-written; only the buffer + transport runtime remain.
export 'src/lcm/lcm_buffer.dart';
export 'src/lcm/lcm.dart';

// Transport
export 'src/channels.dart';
export 'src/transport.dart';

// raccoon_ring SHM transport — Dart FFI bridge around the in-tree
// raccoon_ring (cpp/src/raccoon_ring.c). Use this on Flutter/Dart
// clients that need zero-copy SHM IPC with the C++/Python sides of
// raccoon-transport instead of going over UDP-LCM.
//
// Requires libraccoon_ring_bridge.so on the dlopen() search path. Build
// it via cpp/bridge/build.sh (cross-compiles for aarch64 Pi by default).
export 'src/ring/transport.dart';
export 'src/ring/raccoon_ring_bridge_ffi.dart' show setLibraryPath;

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
