// raccoon_ring — minimal single-producer multi-consumer SHM ring buffer.
//
// Why this exists
// ---------------
// iceoryx2's stateful service-descriptor model proved unreliable on our
// Pi setup. A long-lived reader publishing on ~100 channels makes new
// iceoryx2 nodes fail at NodeBuilder::create() with InternalError, and
// even when subscribes succeed, frames don't always reach the subscriber.
// raccoon_ring side-steps all of that with a deliberately boring design:
//
// One file per channel under /dev/shm/. Producer mmaps it, writes frames
// into a fixed-size slot ring. Subscribers mmap the same file and poll
// the producer sequence counter. There is no service descriptor, no
// daemon, no per-channel handshake. Producers and consumers come and go
// freely — a consumer crash leaves stale memory which the next reader
// simply overwrites. A producer crash means subscribers see no new
// sequence number; when the producer comes back it bumps the sequence
// and writes a fresh init frame. No "marked for destruction" state to
// get stuck in.
//
// Tradeoffs vs iceoryx2
// ---------------------
// * Pure polling, no event-driven receive. We pick a poll interval
//   that matches the desired display rate (~1 ms = 1 kHz max). At
//   typical sensor rates (200 Hz reader, 30-60 Hz UI) the polling
//   overhead is < 1 % of a Pi core.
// * No zero-copy. Producer copies into the slot, subscriber copies out.
//   For raccoon's typical sensor frames (< 100 bytes LCM-encoded) this
//   is single-digit microseconds — well below the 100 Hz publish budget.
// * Single producer per channel. Multiple producers would need a CAS
//   on the sequence counter; we don't need that — raccoon's reader is
//   the sole publisher of every sensor channel, and the bridge is the
//   sole publisher of any command channel it owns.
//
// Safety model
// ------------
// * Per-slot SeqLock: the producer marks a slot as "being written"
//   (seq = 0), writes payload, then publishes the sequence number.
//   Subscribers re-read the seq after the copy to confirm the slot
//   wasn't overwritten mid-copy. On overrun the subscriber skips.
// * Producer never blocks on slow subscribers. Subscribers either
//   keep up or lose frames — exactly the LCM semantics raccoon-lib
//   was originally built on.
// * File magic + version field so a stale ring from a prior incompatible
//   build is detected (subscriber sees mismatch, treats it as empty).

#ifndef RACCOON_RING_H
#define RACCOON_RING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// SHM file layout: ring_hdr_t followed by slot_count slot_t records, each
// of `sizeof(slot_t) + max_payload` bytes. All atomics are uint64_t for
// 64-bit relaxed/release semantics on aarch64 + x86_64.
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
// Big enough for any LCM message in raccoon-transport's catalogue
// (the screen_render / cam_frame channels go up to ~30 KiB; those
// can be created with a larger max_payload at open time).
#define RRB_DEFAULT_SLOT_COUNT   64u
#define RRB_DEFAULT_MAX_PAYLOAD  2048u

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
//
// channel must be a valid path component — slashes are URL-encoded to
// `_2F` internally so `/dev/shm/raccoon_ring_<encoded>` is one flat file.
//
// Returns NULL on hard error (open/mmap failed, channel name invalid).
rrb_writer_t* rrb_writer_create(const char* channel,
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

// Try to receive the next frame. If no new frame is available, returns 1.
// If a frame is available, copies up to `buf_size` bytes into `buf`,
// stores the actual payload length in `*out_len`, and returns 0.
// On overrun (producer lapped us before we could read), the lost frames
// are silently skipped — subscribers always see the most recent frames,
// never wait for old ones.
// Returns -1 on hard error (handle invalid, ring magic broken).
int rrb_reader_recv(rrb_reader_t* r, void* buf, size_t buf_size, size_t* out_len);

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
// -1 on error (n out of range, allocation failed, syscall unsupported).
typedef int (*rrb_multi_handler)(size_t reader_index,
                                 const void* payload, size_t len,
                                 void* user);
int rrb_reader_recv_wait_many(rrb_reader_t* const* readers, size_t n,
                              rrb_multi_handler cb, void* user,
                              int timeout_us);

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
