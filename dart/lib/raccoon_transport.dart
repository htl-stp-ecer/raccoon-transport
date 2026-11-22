/// Message classes and channel names of the raccoon transport, and the C++ transport
/// (shared-memory rings) through dart:ffi.
library raccoon_transport;

// Byte buffer that the message classes encode to and decode from (big-endian).
export 'src/lcm/lcm_buffer.dart';

export 'src/channel_table.dart';
export 'src/contract.dart';
export 'src/contract_check.dart';

// The C++ transport through its C bridge (cpp/bridge): publish and subscribe on the
// shared-memory rings, the same transport the C++ and Python components use.
//
// Requires libraccoon_transport_bridge.so on the dlopen() search path. Build it
// with cpp/bridge/build.sh (cross-compiles for the aarch64 Pi by default).
export 'src/ring/transport.dart';
export 'src/ring/transport_bridge_ffi.dart' show setLibraryPath;

// Generated message types
export 'messages/types/orientation_matrix_t.g.dart';
export 'messages/types/quaternion_t.g.dart';
export 'messages/types/scalar_f_t.g.dart';
export 'messages/types/scalar_i32_t.g.dart';
export 'messages/types/scalar_i8_t.g.dart';
export 'messages/types/screen_render_answer_t.g.dart';
export 'messages/types/screen_render_t.g.dart';
export 'messages/types/message_t.g.dart';
export 'messages/types/string_t.g.dart';
export 'messages/types/vector3f_t.g.dart';
export 'messages/types/cam_blob_t.g.dart';
export 'messages/types/cam_detections_t.g.dart';
export 'messages/types/cam_frame_t.g.dart';
export 'messages/types/cam_stream_ctl_t.g.dart';
export 'messages/types/cam_config_t.g.dart';
