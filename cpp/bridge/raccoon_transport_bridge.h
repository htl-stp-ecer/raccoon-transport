/* C interface of raccoon::Transport, for Dart (dart:ffi) and other languages without a
 * C++ binding. Everything that decides how frames move is in the C++ transport; the
 * bridge only keeps a queue per subscription, because a Dart isolate cannot be called
 * from the transport's thread: the caller polls raccoon_transport_receive().
 *
 * Return codes of raccoon_transport_publish: the values of raccoon::PublishResult
 * (0 Ok, 1 NotInTable, 2 TypeMismatch, 3 TooBig, 4 Closed), -1 for invalid arguments.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__)
#  define RACCOON_BRIDGE_API __attribute__((visibility("default")))
#else
#  define RACCOON_BRIDGE_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Create a transport that serves subscriptions on a thread of its own. */
RACCOON_BRIDGE_API int raccoon_transport_create(void** out_transport);
RACCOON_BRIDGE_API void raccoon_transport_destroy(void* transport);

/* `type_name` may be NULL (the frame size is still checked against the channel table). */
RACCOON_BRIDGE_API int raccoon_transport_publish(void* transport, const char* channel, const char* type_name,
                              const uint8_t* data, size_t len, int deduplicate);

/* Returns 0 and a subscription id, or a non-zero code: 1 the channel is not in the
 * table or does not carry `type_name`, -1 invalid arguments. */
RACCOON_BRIDGE_API int raccoon_transport_subscribe(void* transport, const char* channel, const char* type_name,
                                uint64_t* out_id);
RACCOON_BRIDGE_API void raccoon_transport_unsubscribe(void* transport, uint64_t id);

/* Take the oldest queued frame of a subscription.
 *   0   a frame was copied into `buf`, `*out_len` is its length
 *   1   no frame
 *   2   `buf` is too small: `*out_len` is the size needed, the frame stays queued
 *  -1   unknown subscription
 * A queue holds the latest 64 frames; older ones are dropped. */
RACCOON_BRIDGE_API int raccoon_transport_receive(void* transport, uint64_t id, uint8_t* buf, size_t* out_len,
                              size_t max_len);

/* Name of a publish result code, for log lines. */
RACCOON_BRIDGE_API const char* raccoon_transport_result_name(int code);

#ifdef __cplusplus
}
#endif
