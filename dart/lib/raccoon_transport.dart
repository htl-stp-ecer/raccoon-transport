/// Message classes and channel names of the raccoon transport, with access to the
/// shared-memory rings (RaccoonRingTransport) and a separate UDP-multicast client.
library raccoon_transport;

// Message buffer and the UDP-multicast client. The message classes are written by
// hand (big-endian, no fingerprint); the UDP client does not use shared memory.
export 'src/lcm/lcm_buffer.dart';
export 'src/lcm/lcm.dart';

// Transport
export 'src/channels.dart';
export 'src/channel_table.dart';
export 'src/contract.dart';
export 'src/contract_check.dart';
export 'src/transport.dart';

// raccoon_ring SHM transport — Dart FFI bridge around raccoon_ring
// (cpp/src/raccoon_ring.c). Use this on Flutter/Dart clients that talk to the
// C++ components (stm32-data-reader, raccoon-lib) through shared memory.
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
