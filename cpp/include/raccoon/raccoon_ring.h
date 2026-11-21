// raccoon_ring — shared-memory ring buffers with one writer and any number of readers.
//
// Design
// ------
// One file per channel under /dev/shm. The writer maps it and writes frames into a
// fixed number of slots. Readers map the same file and follow the producer sequence
// counter. There is no service descriptor, no daemon and no per-channel handshake:
// processes come and go freely. A crashed reader leaves nothing behind. A crashed
// writer leaves a ring that readers see no new frames on; a restarted writer
// re-initialises the ring in place and readers re-synchronise.
//
// Behaviour
// ---------
// * Event-driven receive. A writer bumps a futex word in the header after every
//   publish and wakes the readers that are parked on it; a reader can park on the
//   futex words of many rings at once (futex_waitv, Linux 5.16 and later, with a
//   1 ms polling fallback on older kernels).
// * No zero-copy. The writer copies into a slot and the reader copies out. Raccoon's
//   frames are small (sensor values, commands), so the copy costs microseconds.
// * One writer per channel. The exception is the reliable-delivery ACK channel,
//   which every process that acknowledges a command writes to: it uses the opt-in
//   multi-producer mode (rrb_writer_create_multi). Two processes that both create a
//   single-writer ring for the same channel corrupt each other's sequence counter.
// * The file never shrinks. A reader may have a larger ring mapped, and a page
//   beyond a shortened end of file raises SIGBUS in it.
//
// Safety model
// ------------
// * Per-slot seqlock: the writer marks a slot as "being written" (seq = 0), writes
//   the payload, then publishes the sequence number. A reader reads the sequence
//   number again after the copy to confirm that the slot was not overwritten
//   during the copy. On overrun the reader skips to the oldest intact slot.
// * The writer never blocks on slow readers. Readers either keep up or lose
//   frames: a reader always sees the most recent frames and never waits for old
//   ones.
// * The header carries a magic number and a version, so a ring from an
//   incompatible build is detected (the reader treats it as empty). A reader also
//   checks that the geometry in the header fits the file before it indexes into it.

#ifndef RACCOON_RING_H
#define RACCOON_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SHM file layout: ring_hdr_t followed by slot_count slot_t records, each
// of `sizeof(slot_t) + max_payload` bytes (rounded up to 8). The sequence
// counters are 64-bit atomics, the futex words are 32-bit.
//
// The header lives at offset 0 of the mmap region. Both writer and reader
// agree on slot_size by reading max_payload from the header — subscribers
// adapt to whatever the producer chose.

#define RRB_MAGIC      0x52435242u  // "RCRB"
// v3: added waiter_count atomic — readers inc/dec around their
// futex_wait so producers can skip the FUTEX_WAKE syscall entirely
// when nobody is parked. Cuts publisher CPU at high publish rates
// (~2 kHz across reader telemetry channels) since the typical case
// is zero waiters per channel.
#define RRB_VERSION    3u

// Default sizing: 64 slots × 2 KiB payload = ~130 KiB per channel.
// Big enough for any message in raccoon-transport's catalogue except the
// screen_render and cam_frame channels (up to a few hundred KiB); those create
// their ring with a larger max_payload.
#define RRB_DEFAULT_SLOT_COUNT   64u
#define RRB_DEFAULT_MAX_PAYLOAD  2048u

// Header flag: this ring has several writers (see rrb_writer_create_multi).
#define RRB_FLAG_MULTI_PRODUCER 0x1u
// A multi-producer reader waits this long for a claimed-but-unwritten slot
// before it assumes the writer died and skips it.
#define RRB_MP_STALL_NS 2000000ull  /* 2 ms */

typedef struct rrb_writer_s rrb_writer_t;
typedef struct rrb_reader_s rrb_reader_t;

// ---- Writer (one per channel per process) ------------------------------

// Create or attach to the ring for `channel`. If the SHM file does not
// exist yet it's created and the producer initialises the header to
// (slot_count, max_payload). If the file exists with a different layout
// (mismatched slot_count or max_payload, or older version), it is wiped
// and re-initialised — only one producer per channel by design, so the
// previous producer was either us (restart, safe) or an unrelated process
// (which has no business publishing on the same channel anyway).
// The file never shrinks: a ring that is re-created with a smaller geometry
// keeps its size, because readers may have the larger file mapped and would
// get SIGBUS on a page beyond a shortened end of file.
//
// channel must be a valid path component — slashes are URL-encoded to
// `_2F` internally so `/dev/shm/raccoon_ring_<encoded>` is one flat file.
//
// Returns NULL on hard error (open/mmap failed, channel name invalid).
rrb_writer_t* rrb_writer_create(const char* channel,
                                uint32_t slot_count,
                                uint32_t max_payload);

// Multi-producer variant, for channels written by SEVERAL processes (the ACK
// channel: every subscriber process acks into it). Differences to
// rrb_writer_create:
//  * An existing valid multi-producer ring is attached as-is and never wiped,
//    so a second process starting up cannot erase frames the first one wrote.
//    It keeps the geometry its first creator chose.
//  * Slots are claimed with an atomic fetch_add, so two writers never fill the
//    same slot. Readers wait for a claimed slot to be filled.
// Limits: ordering between writers is by claim, a writer that stalls for a
// whole ring lap can have its slot overwritten (use only for small, frequent,
// loss-tolerant frames), and a ring that was created single-producer is
// re-initialised. Returns NULL on hard error.
rrb_writer_t* rrb_writer_create_multi(const char* channel,
                                      uint32_t slot_count,
                                      uint32_t max_payload);

// Publish one frame. Bumps the producer sequence counter, writes payload
// into the next slot. Always succeeds unless `data == NULL` or `len`
// exceeds the ring's max_payload (returns -1 in those cases).
// Returns 0 on success.
int rrb_writer_publish(rrb_writer_t* w, const void* data, size_t len);

// Destroy the writer handle. Does NOT remove the SHM file — subscribers
// keep their mmap valid until they close, and a producer restart re-opens
// the same file in-place.
void rrb_writer_destroy(rrb_writer_t* w);

// ---- Reader (multiple per channel, cross-process safe) -----------------

// Attach to the ring for `channel`. If the SHM file doesn't exist yet,
// the reader is still created (returns a valid handle) and will receive
// nothing until the producer materialises the file — same lazy semantics
// the bridge wants for "subscribe before the publisher starts."
//
// Returns NULL only on irrecoverable error (e.g. /dev/shm unwritable).
rrb_reader_t* rrb_reader_open(const char* channel);

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
// On overrun (producer lapped us before we could read), the lost frames
// are skipped — subscribers always see the most recent frames,
// never wait for old ones.
int rrb_reader_recv(rrb_reader_t* r, void* buf, size_t buf_size, size_t* out_len);

// Drop the frame that the last rrb_reader_recv() reported as RRB_RECV_TOO_BIG.
// Returns 0 when a frame was dropped, 1 when there was none pending.
int rrb_reader_skip(rrb_reader_t* r);

// Largest payload the attached ring can carry, 0 while the reader is not
// attached to a ring. A buffer of this size always fits every frame of the ring.
size_t rrb_reader_max_payload(const rrb_reader_t* r);

// Event-driven receive. Identical contract to rrb_reader_recv except
// that when no data is available, the calling thread sleeps inside a
// futex_wait on the ring's wake_seq word until either:
//   * the producer publishes (wake_seq bumped, FUTEX_WAKE delivered)
//   * `timeout_us` microseconds elapse
//   * the thread is interrupted by a signal
//
// Pass `timeout_us == 0` for a non-blocking call (equivalent to plain
// rrb_reader_recv). Pass a large value (e.g. 50_000) in a poll loop to
// keep CPU at zero while idle but still wake regularly enough that
// shutdown signals get acknowledged.
//
// Returns 0 + frame on success, 1 on timeout / no-data after wake,
// -1 on hard error.
int rrb_reader_recv_wait(rrb_reader_t* r, void* buf, size_t buf_size,
                         size_t* out_len, int timeout_us);

// Multi-channel event-driven wait. Parks the calling thread on the
// wake_seq of EVERY reader in `readers[0..n)` simultaneously using the
// kernel's futex_waitv syscall (Linux 5.16+). Wakes as soon as ANY of
// the channels' producers publish, then drains every channel that has
// data, dispatching frames into the provided per-reader callback.
//
// `cb(reader_index, payload, payload_len, user)` is invoked under the
// caller's thread for each delivered frame; return non-zero to stop
// draining mid-loop.
//
// `n` may be up to 128 (the kernel cap on FUTEX_WAITV is 128 entries).
//
// Returns the number of frames delivered. 0 on clean timeout/no-data,
// -1 on error (n out of range, allocation failed). A kernel without
// futex_waitv (before Linux 5.16) is handled by polling every millisecond
// instead of parking, so the call keeps working with a coarser wake latency.
typedef int (*rrb_multi_handler)(size_t reader_index,
                                 const void* payload, size_t len,
                                 void* user);
int rrb_reader_recv_wait_many(rrb_reader_t* const* readers, size_t n,
                              rrb_multi_handler cb, void* user,
                              int timeout_us);

// Variant of rrb_reader_recv_wait_many that ALSO parks on an external
// "control" uint32_t futex word. The wait returns as soon as ANY of:
//   * a producer publishes on a watched channel
//   * the control word value differs from `control_expected`
//   * `timeout_us` microseconds elapse
//   * the thread is interrupted by a signal
//
// `control` must point to a `_Atomic uint32_t` in MAP_SHARED memory OR
// in the same process as the wakers; callers wake it with
// `atomic_fetch_add_explicit(control, 1, release)` followed by a
// FUTEX_WAKE on the same address. `control_expected` is the value the
// caller snapshotted BEFORE any work that could be observed by the
// wake side (e.g. before swapping in a new subscriber list), so the
// futex_waitv's atomic-compare semantics catch a wake that arrived
// during snapshotting.
//
// Pass `control == NULL` to fall back to plain rrb_reader_recv_wait_many
// behaviour (no control entry, channels only).
//
// Returns identically to rrb_reader_recv_wait_many: number of frames
// delivered (0 on timeout / control-only wake), -1 on error.
// NB: the `control` pointer must point to a `_Atomic uint32_t` in C, or
// `std::atomic<uint32_t>` in C++. We declare it as `uint32_t*` in the
// header to avoid mixing _Atomic (C) and std::atomic (C++); the
// implementation uses C11 atomics on the address regardless.
int rrb_reader_recv_wait_many_with_control(rrb_reader_t* const* readers, size_t n,
                                           uint32_t* control,
                                           uint32_t control_expected,
                                           rrb_multi_handler cb, void* user,
                                           int timeout_us);

// Mark the reader so its NEXT rrb_reader_recv() call returns the most
// recently published frame in the ring (if any), instead of walking
// from the oldest still-intact slot. Use this to implement
// "retain-on-attach" / latest-value-on-subscribe semantics for fresh
// subscribers on slow- or dedup-filtered channels where waiting for a
// brand-new publish would block longer than the caller's timeout.
//
// No-op when the producer has not yet materialised the SHM file
// (returns 0 — the next recv() lazy-attaches and finds an empty ring).
// Returns -1 only on invalid handle.
int rrb_reader_seek_to_latest(rrb_reader_t* r);

void rrb_reader_close(rrb_reader_t* r);

// ---- Utilities for tests + diagnostics ---------------------------------

// Compute the SHM file path for a given channel. Useful for cleanup
// scripts ("rm -f /dev/shm/raccoon_ring_*"). out_path must be at least
// 512 bytes. Returns 0 on success, -1 if channel is invalid.
int rrb_channel_to_path(const char* channel, char* out_path, size_t out_size);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // RACCOON_RING_H
