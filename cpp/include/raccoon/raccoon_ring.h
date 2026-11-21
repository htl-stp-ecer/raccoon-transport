// raccoon_ring — shared-memory ring buffers, any number of writers and readers.
//
// Design
// ------
// One file per channel under /dev/shm/raccoon-rings/. A ring is a header followed by
// a fixed number of slots; every slot has the same size, which the channel table
// fixes per channel. The first process that opens a channel (reader or writer)
// creates the file with that geometry, under an flock, and every later process
// attaches to it. There is no daemon, no handshake and no placeholder file:
// processes come and go freely, and a crashed process leaves nothing behind but the
// ring, which stays valid.
//
// Every ring accepts several writers. A writer takes a slot ticket with an atomic
// fetch_add, fills the slot and publishes it; readers wait for a claimed slot to be
// filled.
//
// Behaviour
// ---------
// * Event-driven receive. A writer bumps a futex word in the header after every
//   publish and wakes the readers that are parked on it; a reader can park on the
//   futex words of many rings at once (futex_waitv, Linux 5.16 and later, with a
//   1 ms polling fallback on older kernels).
// * No zero-copy. The writer copies into a slot and the reader copies out.
// * A new reader starts after the latest frame. A reader of a retained channel
//   receives the latest frame once, then the frames that follow.
// * The file never shrinks. A reader may have a larger ring mapped, and a page
//   beyond a shortened end of file raises SIGBUS in it.
// * Files have mode 0666 and the directory 0777, so every user on the machine can
//   use every channel.
//
// Safety model
// ------------
// * Per-slot seqlock: a writer marks a slot as "being written" (seq = 0), writes
//   the payload, then publishes the sequence number. A reader reads the sequence
//   number again after the copy to confirm that the slot was not overwritten
//   during the copy. On overrun the reader skips to the oldest intact slot.
// * A writer never blocks on slow readers. Readers either keep up or lose
//   frames: a reader always sees the most recent frames and never waits for old
//   ones.
// * The header carries a magic number and a version, so a ring from an
//   incompatible build is never used. A reader also checks that the geometry in the
//   header fits the file before it indexes into it.

#ifndef RACCOON_RING_H
#define RACCOON_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RRB_MAGIC      0x52435242u  // "RCRB"
// v4: every ring is multi-producer, the slot stride is a multiple of 64 bytes,
// files live in /dev/shm/raccoon-rings/ and the header holds only the geometry
// and flags.
#define RRB_VERSION    4u

// Directory of the ring files, and the longest channel name (the header keeps it
// for diagnostics).
#define RRB_DIR              "/dev/shm/raccoon-rings"
#define RRB_MAX_CHANNEL_NAME 127

// Header flag: the channel is retained, a new reader receives its latest frame.
#define RRB_FLAG_RETAINED 0x1u

// A reader waits this long for a claimed-but-unwritten slot before it assumes the
// writer died and skips it.
#define RRB_MP_STALL_NS 2000000ull  /* 2 ms */

// Bytes one slot occupies for a payload of `max_payload` bytes: the 16-byte slot
// header plus the payload, rounded up to a multiple of 64 (a cache line on the
// Cortex-A53).
size_t rrb_slot_stride(uint32_t max_payload);

// Bytes of a ring file for the given geometry.
size_t rrb_ring_bytes(uint32_t slot_count, uint32_t max_payload);

typedef struct rrb_writer_s rrb_writer_t;
typedef struct rrb_reader_s rrb_reader_t;

// ---- Writer (one per channel per process) ------------------------------

// Attach to the ring of `channel`, creating it with (slot_count, max_payload) if
// it does not exist. An existing ring with the same geometry is attached as-is and
// never wiped, so a second process starting up cannot erase frames the first one
// wrote. A ring with another geometry (a leftover of a different channel table)
// is re-initialised in place. Returns NULL on a hard error (invalid channel name,
// /dev/shm not writable).
rrb_writer_t* rrb_writer_open(const char* channel,
                              uint32_t slot_count,
                              uint32_t max_payload);

// Return codes of rrb_writer_publish.
#define RRB_PUBLISH_TOO_BIG (-2)

// Publish one frame: claim the next slot, copy the payload, wake the readers.
// Returns 0 on success, RRB_PUBLISH_TOO_BIG when `len` exceeds the ring's
// max_payload, -1 for invalid arguments.
int rrb_writer_publish(rrb_writer_t* w, const void* data, size_t len);

// Destroy the writer handle. Does NOT remove the ring file: readers keep their
// mapping and the next writer attaches to the same file.
void rrb_writer_destroy(rrb_writer_t* w);

// ---- Reader (multiple per channel, cross-process safe) -----------------

// Attach to the ring of `channel`, creating it with (slot_count, max_payload) if
// it does not exist. The reader starts after the latest frame; with
// RRB_FLAG_RETAINED in `flags` it receives the latest frame once. Returns NULL on
// a hard error (invalid channel name, /dev/shm not writable).
rrb_reader_t* rrb_reader_open(const char* channel,
                              uint32_t slot_count,
                              uint32_t max_payload,
                              uint32_t flags);

// Return code of rrb_reader_recv for a frame that does not fit the buffer.
#define RRB_RECV_TOO_BIG 2

// Try to receive the next frame.
//   0                  A frame was copied into `buf`; `*out_len` is its length.
//   1                  No new frame.
//   RRB_RECV_TOO_BIG   The next frame is larger than `buf_size`. Nothing is
//                      copied and the frame is NOT consumed; `*out_len` is the
//                      buffer size it needs. Call again with a buffer of that
//                      size, or call rrb_reader_skip() to drop the frame.
//   -1                 Hard error (invalid handle).
// On overrun (writers lapped the reader), the lost frames are skipped: a reader
// always sees the most recent frames, it never waits for old ones.
int rrb_reader_recv(rrb_reader_t* r, void* buf, size_t buf_size, size_t* out_len);

// Drop the frame that the last rrb_reader_recv() reported as RRB_RECV_TOO_BIG.
// Returns 0 when a frame was dropped, 1 when there was none pending.
int rrb_reader_skip(rrb_reader_t* r);

// Largest payload the attached ring can carry. A buffer of this size always fits
// every frame of the ring.
size_t rrb_reader_max_payload(const rrb_reader_t* r);

// Event-driven receive. Identical contract to rrb_reader_recv except that when no
// frame is available, the calling thread sleeps on the ring's wake_seq futex until
// a writer publishes, `timeout_us` microseconds elapse or a signal arrives.
// `timeout_us <= 0` does not block.
// Returns 0 + frame on success, 1 on timeout / no frame, -1 on hard error.
int rrb_reader_recv_wait(rrb_reader_t* r, void* buf, size_t buf_size,
                         size_t* out_len, int timeout_us);

// Multi-channel event-driven wait. Parks the calling thread on the wake_seq of
// EVERY reader in `readers[0..n)` at once (futex_waitv, Linux 5.16+). Wakes as soon
// as ANY of the channels publishes, then drains every channel that has frames and
// hands them to `cb(reader_index, payload, payload_len, user)`; a non-zero return
// of `cb` stops the draining.
//
// `n` may be up to 128 (the kernel cap on futex_waitv entries; 127 with a control
// word).
//
// Returns the number of frames delivered, 0 on a clean timeout, -1 on error. A
// kernel without futex_waitv (before Linux 5.16) is handled by polling every
// millisecond instead of parking.
typedef int (*rrb_multi_handler)(size_t reader_index,
                                 const void* payload, size_t len,
                                 void* user);
int rrb_reader_recv_wait_many(rrb_reader_t* const* readers, size_t n,
                              rrb_multi_handler cb, void* user,
                              int timeout_us);

// Variant that ALSO parks on an external "control" uint32_t futex word. The wait
// returns as soon as ANY of:
//   * a writer publishes on a watched channel
//   * the control word differs from `control_expected`
//   * `timeout_us` microseconds elapse
//   * the thread is interrupted by a signal
// `control` points to a 32-bit atomic in memory shared with the wakers; a waker
// increments it and issues a FUTEX_WAKE on the same address. `control_expected` is
// the value the caller read BEFORE any work that the waker could observe, so that
// a wake during that work is not lost. `control == NULL` waits on the channels only.
int rrb_reader_recv_wait_many_with_control(rrb_reader_t* const* readers, size_t n,
                                           uint32_t* control,
                                           uint32_t control_expected,
                                           rrb_multi_handler cb, void* user,
                                           int timeout_us);

// Check that the reader is still attached to the file that is on disk: a ring file
// that was deleted and created again (a cleanup of /dev/shm, say) leaves the reader
// with a mapping nobody writes to. The reader then attaches to the new file. The
// waiting calls above do this about once a second while they are idle; call it
// yourself when a reader is polled with rrb_reader_recv only. Returns 0 when the
// reader is attached, -1 when it could not attach.
int rrb_reader_revalidate(rrb_reader_t* r);

void rrb_reader_close(rrb_reader_t* r);

// ---- Utilities for tests + diagnostics ---------------------------------

// The file path of a channel: RRB_DIR plus the channel name with every '/'
// written as '.'. Channel names consist of 0-9 a-z A-Z _ - and '/' between
// non-empty segments, so the mapping is unambiguous. out_path should hold at least
// 256 bytes. Returns 0 on success, -1 if the name is invalid.
int rrb_channel_to_path(const char* channel, char* out_path, size_t out_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // RACCOON_RING_H
