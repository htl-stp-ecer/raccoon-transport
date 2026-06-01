// raccoon_ring.c — implementation. See raccoon_ring.h for design rationale.

// Need _POSIX_C_SOURCE for ftruncate visibility under -std=c11, plus
// _GNU_SOURCE for syscall() under glibc (it's in <unistd.h> only when
// the GNU feature test macro is on).
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE
#include "raccoon/raccoon_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// ---- Futex glue --------------------------------------------------------
//
// We use raw FUTEX_WAIT / FUTEX_WAKE on a uint32_t living in the ring
// header so producer wakes propagate to subscribers in another process
// with sub-microsecond latency. The futex word is in shared (MAP_SHARED)
// memory — the kernel hashes by physical address, so cross-process wakes
// work without exchanging fds or pid info.
//
// FUTEX_WAIT semantics: the kernel atomically compares *uaddr to `val`
// at wait time and only sleeps if they match. If a publish bumped the
// value between the subscriber's snapshot read and the wait call, the
// wait returns EAGAIN immediately — no lost wakeup race.

static int futex_wait_until(_Atomic uint32_t* uaddr, uint32_t expected,
                            int timeout_us) {
    struct timespec ts = {
        timeout_us / 1000000,
        (long)(timeout_us % 1000000) * 1000L,
    };
    // syscall(SYS_futex, addr, op, val, timeout, uaddr2, val3)
    return (int)syscall(SYS_futex, (uint32_t*)uaddr, FUTEX_WAIT,
                        expected, &ts, NULL, 0);
}

static int futex_wake_all(_Atomic uint32_t* uaddr) {
    return (int)syscall(SYS_futex, (uint32_t*)uaddr, FUTEX_WAKE,
                        INT_MAX, NULL, NULL, 0);
}

// futex_waitv glue (Linux 5.16+). Kernel headers from older toolchains
// may not declare these — define them locally so we don't ride on a
// linux-headers package being present at build time. Values from
// include/uapi/linux/futex.h.

#ifndef SYS_futex_waitv
#  define SYS_futex_waitv 449  /* arm64/x86_64 syscall number */
#endif
#ifndef FUTEX2_SIZE_U32
#  define FUTEX2_SIZE_U32 0x02
#endif
#ifndef FUTEX2_PRIVATE
#  define FUTEX2_PRIVATE  0x80
#endif

struct rrb_futex_waitv {
    uint64_t val;
    uint64_t uaddr;
    uint32_t flags;
    uint32_t __reserved;
};

// Returns the kernel's "which entry woke me" index on a wake, -1 with
// errno=ETIMEDOUT on timeout, -1 with errno otherwise.
static int futex_waitv_until(struct rrb_futex_waitv* waiters, size_t n,
                             int timeout_us) {
    if (n == 0) return -1;
    // CLOCK_MONOTONIC abstime for the syscall
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t total_ns = (int64_t)ts.tv_nsec + (int64_t)timeout_us * 1000LL;
    ts.tv_sec += total_ns / 1000000000LL;
    ts.tv_nsec = total_ns % 1000000000LL;
    // clockid 1 = CLOCK_MONOTONIC
    return (int)syscall(SYS_futex_waitv, waiters, (unsigned)n,
                        /*flags=*/0u, &ts, /*clockid=*/1);
}

// ---- On-disk layout ----------------------------------------------------

#pragma pack(push, 8)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_size;        // sizeof(slot_hdr) + max_payload, rounded up
    uint32_t max_payload;
    // wake_seq is the futex word producers bump after every successful
    // publish. Subscribers FUTEX_WAIT on it for event-driven receive
    // instead of polling — drops idle CPU to 0 and per-publish wake
    // latency to ~us. uint32_t (futex requirement). Placed BEFORE
    // producer_seq so its address is 8-byte aligned and the producer_seq
    // 64-bit atomic stays 8-aligned right after.
    _Atomic uint32_t wake_seq;
    // v3 addition. Readers inc this before parking in futex_wait and
    // dec after returning; producers skip the FUTEX_WAKE syscall when
    // it's 0. The atomic itself is acquire/release-ordered so the
    // producer's load can't speculatively reorder past the publish.
    _Atomic uint32_t waiter_count;
    uint32_t _pad0;                  // align producer_seq to 8 bytes
    _Atomic uint64_t producer_seq;  // monotonic, slot_idx = (seq-1) % count
    char     channel_name[128];
    uint8_t  pad[88];  // align header to 256 bytes (40 + 128 + 88 = 256)
} ring_hdr_t;

typedef struct {
    _Atomic uint64_t seq;      // 0 = being written / empty; else = producer seq when published
    uint32_t len;              // payload bytes valid
    uint32_t _reserved;
    uint8_t  data[];           // max_payload bytes
} slot_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(ring_hdr_t) == 256, "header must be 256 bytes");
_Static_assert(sizeof(slot_hdr_t) == 16,  "slot header must be 16 bytes");

// Round up to multiple of `align` (power of two not required here).
static size_t round_up(size_t v, size_t align) {
    return ((v + align - 1) / align) * align;
}

static size_t slot_stride(uint32_t max_payload) {
    // 8-byte align so the next slot's _Atomic uint64_t seq is aligned.
    return round_up(sizeof(slot_hdr_t) + max_payload, 8);
}

static size_t file_size_for(uint32_t slot_count, uint32_t max_payload) {
    return sizeof(ring_hdr_t) + (size_t)slot_count * slot_stride(max_payload);
}

// ---- Path encoding -----------------------------------------------------

// raccoon channel names contain slashes ("raccoon/accel/value"). /dev/shm
// entries are flat: a slash starts a subdirectory which we don't want to
// create per-channel. Encode slashes as `_2F` (URL-style) and refuse any
// other character that's awkward in shell globs or file APIs.

int rrb_channel_to_path(const char* channel, char* out_path, size_t out_size) {
    if (!channel || !out_path || out_size < 32) return -1;
    const char* prefix = "/dev/shm/raccoon_ring_";
    size_t pl = strlen(prefix);
    if (pl >= out_size) return -1;
    memcpy(out_path, prefix, pl);
    size_t op = pl;
    for (const char* c = channel; *c; ++c) {
        char ch = *c;
        if (ch == '/') {
            if (op + 3 >= out_size) return -1;
            out_path[op++] = '_';
            out_path[op++] = '2';
            out_path[op++] = 'F';
            continue;
        }
        // Allow alnum + a few harmless punctuation; reject everything else
        // so we never end up with a path-traversal-shaped filename.
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) {
            return -1;
        }
        if (op + 1 >= out_size) return -1;
        out_path[op++] = ch;
    }
    out_path[op] = '\0';
    return 0;
}

// ---- Common: open shm, mmap header, return base+size -------------------

// `create`: 0 = open existing readonly; 1 = open RW + create if missing.
// Returns 0 on success, -1 on failure. `*out_base` is the mmap region,
// `*out_size` the file size, `*out_fd` the still-open fd (must be closed
// after mmap by the caller is fine on Linux — kernel keeps mapping alive).
static int shm_open_mmap(const char* path,
                         int create,
                         uint32_t want_slot_count,
                         uint32_t want_max_payload,
                         const char* channel_name,
                         void** out_base,
                         size_t* out_size,
                         int* out_fd) {
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    // RW even for readers: we don't need to write payload, but mapping has
    // to be MAP_SHARED + PROT_READ to see producer's atomic stores. Some
    // libcs require RDWR on the fd for MAP_SHARED writable mappings; we
    // keep things simple by always opening RDWR.
    int fd = open(path, flags, 0660);
    if (fd < 0) {
        if (!create && errno == ENOENT) {
            // Reader before any producer: not an error, caller handles by
            // returning a "lazy" handle.
            return -2;
        }
        return -1;
    }

    size_t want_size = file_size_for(want_slot_count, want_max_payload);

    if (create) {
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); return -1; }
        // Either fresh file (size 0) or stale from earlier run. Always
        // ftruncate to our wanted size and re-initialise the header — only
        // one producer per channel, so a stale file means whoever made it
        // is either us or doesn't matter.
        if ((size_t)st.st_size != want_size) {
            if (ftruncate(fd, (off_t)want_size) != 0) {
                close(fd); return -1;
            }
        }
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); return -1; }
        want_size = (size_t)st.st_size;
        if (want_size < sizeof(ring_hdr_t)) {
            // Producer hasn't initialised yet. Caller treats as lazy.
            close(fd);
            return -2;
        }
    }

    void* base = mmap(NULL, want_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return -1; }

    if (create) {
        // Initialise (or re-initialise) the header. Writers re-initialise
        // on every restart — that's fine, the .seq starts back at 0 and
        // subscribers naturally lose their old last_seen reference.
        ring_hdr_t* h = (ring_hdr_t*)base;
        h->magic       = RRB_MAGIC;
        h->version     = RRB_VERSION;
        h->slot_count  = want_slot_count;
        h->slot_size   = (uint32_t)slot_stride(want_max_payload);
        h->max_payload = want_max_payload;
        // (wake_seq replaced the old _reserved0 padding slot.)
        atomic_store_explicit(&h->producer_seq, 0, memory_order_release);
        atomic_store_explicit(&h->wake_seq, 0u, memory_order_release);
        atomic_store_explicit(&h->waiter_count, 0u, memory_order_release);
        strncpy(h->channel_name, channel_name, sizeof(h->channel_name) - 1);
        h->channel_name[sizeof(h->channel_name) - 1] = '\0';
        // Zero every slot's seq field so the first publish wraps cleanly.
        uint8_t* slot_base = (uint8_t*)base + sizeof(ring_hdr_t);
        size_t stride = h->slot_size;
        for (uint32_t i = 0; i < want_slot_count; ++i) {
            slot_hdr_t* s = (slot_hdr_t*)(slot_base + (size_t)i * stride);
            atomic_store_explicit(&s->seq, 0, memory_order_release);
            s->len = 0;
        }
    }

    *out_base = base;
    *out_size = want_size;
    *out_fd   = fd;
    return 0;
}

// ---- Writer ------------------------------------------------------------

struct rrb_writer_s {
    char        path[512];
    int         fd;
    void*       base;
    size_t      size;
    ring_hdr_t* hdr;
    uint8_t*    slots;     // points to slot 0
    size_t      stride;    // slot_size from header
};

rrb_writer_t* rrb_writer_create(const char* channel,
                                uint32_t slot_count,
                                uint32_t max_payload) {
    if (!channel || slot_count == 0 || max_payload == 0) return NULL;

    rrb_writer_t* w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    if (rrb_channel_to_path(channel, w->path, sizeof(w->path)) != 0) {
        free(w); return NULL;
    }
    int rc = shm_open_mmap(w->path, /*create=*/1, slot_count, max_payload,
                           channel, &w->base, &w->size, &w->fd);
    if (rc != 0) { free(w); return NULL; }

    w->hdr   = (ring_hdr_t*)w->base;
    w->slots = (uint8_t*)w->base + sizeof(ring_hdr_t);
    w->stride = w->hdr->slot_size;
    return w;
}

int rrb_writer_publish(rrb_writer_t* w, const void* data, size_t len) {
    if (!w || !data) return -1;
    if (len > w->hdr->max_payload) return -1;

    // 1) Pick the next sequence number. Single-producer: just += 1.
    //    seq 1 lives in slot 0, seq 2 in slot 1, ..., seq N in slot N-1.
    uint64_t cur = atomic_load_explicit(&w->hdr->producer_seq,
                                        memory_order_relaxed);
    uint64_t next = cur + 1;
    uint32_t slot_idx = (uint32_t)((next - 1) % w->hdr->slot_count);
    slot_hdr_t* s = (slot_hdr_t*)(w->slots + (size_t)slot_idx * w->stride);

    // 2) Tag slot as in-progress so any reader currently copying it sees
    //    the slot's seq go to 0 and bails out.
    atomic_store_explicit(&s->seq, 0, memory_order_release);

    // 3) Write payload.
    s->len = (uint32_t)len;
    memcpy(s->data, data, len);

    // 4) Publish slot (release fence pairs with subscriber's acquire load
    //    of slot->seq below).
    atomic_store_explicit(&s->seq, next, memory_order_release);

    // 5) Make the new producer_seq visible to subscribers polling the
    //    global counter. Release order: a reader observing producer_seq
    //    == next must afterwards see slot->seq == next too.
    atomic_store_explicit(&w->hdr->producer_seq, next, memory_order_release);

    // 6) Bump the futex word and wake every subscriber currently parked
    //    in rrb_reader_recv_wait(). The increment-then-wake order
    //    matches the futex protocol: subscribers snapshot wake_seq,
    //    re-check producer_seq, and FUTEX_WAIT(expected=snapshot). If
    //    we bump the value between their snapshot and their wait call
    //    the kernel returns EAGAIN — no lost wakeup. FUTEX_WAKE on a
    //    word with no waiters is ~50 ns; cheap enough to do every
    //    publish even at 1 kHz channel rates.
    atomic_fetch_add_explicit(&w->hdr->wake_seq, 1u,
                              memory_order_release);
    // Skip the FUTEX_WAKE syscall when nobody is parked. Telemetry
    // channels publish at ~100 Hz across ~20 channels; a Pi spending
    // 2 kHz/s on no-op syscalls is measurable. The waiter_count
    // load is acquire so it can't be reordered past the publish
    // (matching the reader's release-ordered increment below — if
    // they see our publish, we see their bump or they see our wake).
    if (atomic_load_explicit(&w->hdr->waiter_count,
                             memory_order_acquire) > 0u) {
        (void)futex_wake_all(&w->hdr->wake_seq);
    }
    return 0;
}

void rrb_writer_destroy(rrb_writer_t* w) {
    if (!w) return;
    if (w->base && w->base != MAP_FAILED) munmap(w->base, w->size);
    if (w->fd >= 0) close(w->fd);
    // Intentionally NOT unlinking the SHM file — subscribers that mmap'd
    // it keep working until they close, and a producer restart re-uses
    // the same file in-place via shm_open_mmap(create=1).
    free(w);
}

// ---- Reader ------------------------------------------------------------

struct rrb_reader_s {
    char        path[512];
    char        channel[128];
    int         fd;
    void*       base;
    size_t      size;
    ring_hdr_t* hdr;
    uint8_t*    slots;
    size_t      stride;
    uint64_t    last_seen;   // last delivered producer_seq
    uint64_t    last_open_attempt_ns;  // for lazy re-attach throttling
};

// Detach from the current mmap (if any) and reset fields so the next
// recv() call triggers a fresh reader_attach(). Safe to call even when
// the reader is not attached.
static void reader_detach(rrb_reader_t* r) {
    if (r->base && r->base != MAP_FAILED) munmap(r->base, r->size);
    if (r->fd >= 0) close(r->fd);
    r->base   = NULL;
    r->size   = 0;
    r->hdr    = NULL;
    r->slots  = NULL;
    r->stride = 0;
    r->fd     = -1;
    r->last_seen = 0;
}

// Try to attach. Sets the mmap fields if successful; returns 0 on
// success, -1 on hard error.
//
// When the file doesn't exist yet we stay detached and let recv() retry.
// Readers must not create placeholder rings: if a real producer later
// materialises the channel with a smaller layout, its ftruncate can shrink
// the file under the reader's larger mmap and SIGBUS the next access.
static int reader_attach(rrb_reader_t* r) {
    int fd = -1;
    void* base = NULL;
    size_t size = 0;
    int rc = shm_open_mmap(r->path, /*create=*/0, 0, 0, r->channel,
                           &base, &size, &fd);
    if (rc != 0) {
        return rc;
    }

    ring_hdr_t* h = (ring_hdr_t*)base;
    if (h->magic != RRB_MAGIC || h->version != RRB_VERSION) {
        // Stale ring from an incompatible build, or file is being
        // written by the producer right now. Treat as lazy.
        munmap(base, size);
        close(fd);
        return -2;
    }
    r->fd     = fd;
    r->base   = base;
    r->size   = size;
    r->hdr    = h;
    r->slots  = (uint8_t*)base + sizeof(ring_hdr_t);
    r->stride = h->slot_size;
    return 0;
}

// Check whether the underlying SHM file has been unlinked (st_nlink == 0)
// and, if a replacement file exists at the same path, detach from the
// orphaned mmap and re-attach to the fresh inode. Returns 0 if still
// attached (no action taken or re-attach succeeded), -1 on error.
//
// This handles producers (e.g. stm32-data-reader) that create the ring
// file and then immediately unlink it — a common POSIX pattern that
// keeps the producer's own fd alive but prevents new readers from
// opening the directory entry. When the producer restarts and creates a
// fresh file with the same path, old readers see no new data because
// their mmap points at the deleted (orphaned) inode.  Checking
// st_nlink == 0 catches this and triggers a transparent re-attach.
static int reader_reopen_if_unlinked(rrb_reader_t* r) {
    if (r->fd < 0) return -1;
    struct stat st;
    if (fstat(r->fd, &st) != 0) return -1;
    if (st.st_nlink > 0) return 0;  // still reachable — nothing to do

    // File was unlinked. Detach from the orphaned mmap and re-attach.
    // reader_attach() will ENOENT => lazy if the new producer hasn't
    // materialised yet; the next recv() call retries as usual.
    reader_detach(r);
    int rc = reader_attach(r);
    return (rc == 0) ? 0 : -1;
}

rrb_reader_t* rrb_reader_open(const char* channel) {
    if (!channel) return NULL;
    rrb_reader_t* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (rrb_channel_to_path(channel, r->path, sizeof(r->path)) != 0) {
        free(r); return NULL;
    }
    strncpy(r->channel, channel, sizeof(r->channel) - 1);
    r->fd = -1;
    // Try to attach now; if file doesn't exist yet, leave r->base NULL
    // and let recv() retry. The caller doesn't need to care.
    (void)reader_attach(r);
    return r;
}

int rrb_reader_recv(rrb_reader_t* r, void* buf, size_t buf_size, size_t* out_len) {
    if (!r || !buf || !out_len) return -1;
    *out_len = 0;

    // Lazy attach if the producer hadn't materialised the file yet.
    if (!r->base) {
        if (reader_attach(r) != 0) return 1;
    }

    for (int attempt = 0; attempt < 8; ++attempt) {
        uint64_t producer = atomic_load_explicit(&r->hdr->producer_seq,
                                                 memory_order_acquire);
        if (producer == r->last_seen) {
            // No new data.  The underlying file may have been unlinked
            // and replaced since we last attached — if so, re-attach so
            // the next poll sees the fresh inode.
            reader_reopen_if_unlinked(r);
            if (r->base) {
                producer = atomic_load_explicit(&r->hdr->producer_seq,
                                                memory_order_acquire);
            }
            if (producer == r->last_seen) return 1;
        }

        // Producer restarted: producer_seq moved backwards because
        // rrb_writer_create re-initialised the ring in place. Reset to
        // the START of the new epoch so we replay every still-intact frame
        // from the restarted producer, not just the current tail.
        if (producer < r->last_seen) {
            r->last_seen = 0;
            if (producer == 0) return 1;
        }

        // If we're behind by more than (slot_count - 1), the producer has
        // lapped us. Jump to the oldest still-intact slot in the ring.
        uint64_t next;
        if (producer - r->last_seen > (uint64_t)r->hdr->slot_count - 1u) {
            next = producer - ((uint64_t)r->hdr->slot_count - 1u);
        } else {
            next = r->last_seen + 1;
        }

        uint32_t slot_idx = (uint32_t)((next - 1) % r->hdr->slot_count);
        slot_hdr_t* s = (slot_hdr_t*)(r->slots + (size_t)slot_idx * r->stride);

        // SeqLock read: snapshot seq, copy payload, re-check seq. If it
        // changed, the slot was overwritten by the producer mid-copy and we
        // must move on — same data loss policy as overrun above.
        for (int retry = 0; retry < 3; ++retry) {
            uint64_t seq_before = atomic_load_explicit(&s->seq, memory_order_acquire);
            if (seq_before == 0 || seq_before < next) {
                // Producer cleared the slot to write something new; treat as
                // overrun, bump last_seen so we move past this seq.
                r->last_seen = next;
                return 1;
            }
            if (seq_before > next) {
                // Producer already wrote a NEWER seq into this slot since we
                // looked. Advance to that seq and retry from the top without
                // recursion; after a producer restart this may briefly point at
                // stale high seq numbers from the prior epoch.
                r->last_seen = seq_before - 1;
                break;
            }
            // seq_before == next — slot looks valid. Copy payload.
            uint32_t len = s->len;
            if (len > r->hdr->max_payload) len = r->hdr->max_payload;
            size_t copy_len = len > buf_size ? buf_size : len;
            memcpy(buf, s->data, copy_len);

            // Verify seq didn't change during the copy.
            uint64_t seq_after = atomic_load_explicit(&s->seq, memory_order_acquire);
            if (seq_after == next) {
                *out_len = copy_len;
                r->last_seen = next;
                return 0;
            }
            // Slot was overwritten during our copy — retry with the newer seq.
        }
    }
    // The producer kept moving while we were inspecting the ring, or we
    // saw an inconsistent restart boundary. Let the next poll retry from
    // the updated last_seen state instead of recursing indefinitely.
    return 1;
}

int rrb_reader_recv_wait(rrb_reader_t* r, void* buf, size_t buf_size,
                         size_t* out_len, int timeout_us) {
    if (!r || !buf || !out_len) return -1;

    // Fast path: try once before parking. Covers the common case where
    // the producer published while we were processing the last frame —
    // no syscall needed.
    int rc = rrb_reader_recv(r, buf, buf_size, out_len);
    if (rc != 1) return rc;          // 0 = got data, -1 = error
    if (timeout_us <= 0) return 1;   // caller wanted non-blocking

    // Park on the producer's wake_seq. The futex protocol guarantees
    // no lost wakeups: if the producer bumps wake_seq between our
    // snapshot and the syscall, the kernel returns EAGAIN immediately
    // and we re-poll.
    //
    // We only need r->base mapped to read wake_seq. If the producer
    // hasn't materialised the file yet (lazy attach failed inside
    // rrb_reader_recv above), wait a few ms and try again.
    if (!r->base) {
        struct timespec lazy = {0, 1000000L};  // 1 ms
        nanosleep(&lazy, NULL);
        return rrb_reader_recv(r, buf, buf_size, out_len);
    }

    // Bump waiter_count BEFORE snapshotting wake_seq so the writer's
    // skip check (load waiter_count, then publish) can't miss us: if
    // the writer's load happens-before our increment, our snapshot of
    // wake_seq will already include the writer's publish and the
    // futex_wait returns EAGAIN immediately.
    atomic_fetch_add_explicit(&r->hdr->waiter_count, 1u,
                              memory_order_release);
    uint32_t expected = atomic_load_explicit(&r->hdr->wake_seq,
                                             memory_order_acquire);
    (void)futex_wait_until(&r->hdr->wake_seq, expected, timeout_us);
    atomic_fetch_sub_explicit(&r->hdr->waiter_count, 1u,
                              memory_order_release);
    // EAGAIN, EINTR, ETIMEDOUT all fall through — re-poll either way.
    return rrb_reader_recv(r, buf, buf_size, out_len);
}

int rrb_reader_recv_wait_many(rrb_reader_t* const* readers, size_t n,
                              rrb_multi_handler cb, void* user,
                              int timeout_us) {
    return rrb_reader_recv_wait_many_with_control(readers, n, NULL, 0,
                                                  cb, user, timeout_us);
}

int rrb_reader_recv_wait_many_with_control(rrb_reader_t* const* readers, size_t n,
                                           uint32_t* control,
                                           uint32_t control_expected,
                                           rrb_multi_handler cb, void* user,
                                           int timeout_us) {
    // 128 is the kernel's futex_waitv cap. We need 1 slot for the optional
    // control entry, so channels can use up to 127 when control is set.
    if (!readers || !cb || n == 0) return -1;
    const size_t max_channels = control ? 127 : 128;
    if (n > max_channels) return -1;

    // Tiny scratch buffer used by the drain loop — sized to RRB_DEFAULT
    // for the common case and grown to the largest ring's max_payload
    // we see attached. 8 KiB is plenty for non-camera channels.
    uint8_t scratch[8192];
    size_t scratch_sz = sizeof(scratch);
    uint8_t* big = NULL;
    size_t big_sz = 0;

    // Pass 1: fast non-blocking drain across every reader. Most spins
    // catch frames here without ever calling the syscall.
    int delivered = 0;
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r || !r->base) continue;
        for (;;) {
            size_t out_len = 0;
            uint8_t* buf = scratch;
            size_t buf_sz = scratch_sz;
            if (r->hdr->max_payload > scratch_sz) {
                if (r->hdr->max_payload > big_sz) {
                    free(big);
                    big_sz = r->hdr->max_payload;
                    big = (uint8_t*)malloc(big_sz);
                    if (!big) { delivered = -1; goto done; }
                }
                buf = big;
                buf_sz = big_sz;
            }
            int rc = rrb_reader_recv(r, buf, buf_sz, &out_len);
            if (rc != 0) break;
            if (cb(i, buf, out_len, user) != 0) { goto done; }
            ++delivered;
        }
    }
    if (delivered > 0 || timeout_us == 0) goto done;

    // Pass 2: build the waitv list and park. Only readers that are
    // attached and whose wake_seq is observable get added; un-attached
    // ones (file doesn't exist yet) skip the wait and are caught by
    // the next caller's pass 1. Optional control word goes in last so
    // the caller can identify a "wake came from control, not a
    // channel" event by re-reading *control and comparing to
    // control_expected.
    struct rrb_futex_waitv waiters[128];
    size_t nw = 0;
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r || !r->base) continue;
        waiters[nw].val = atomic_load_explicit(&r->hdr->wake_seq,
                                               memory_order_acquire);
        waiters[nw].uaddr = (uint64_t)(uintptr_t)&r->hdr->wake_seq;
        waiters[nw].flags = FUTEX2_SIZE_U32;
        waiters[nw].__reserved = 0;
        ++nw;
    }
    if (control) {
        waiters[nw].val = control_expected;
        waiters[nw].uaddr = (uint64_t)(uintptr_t)control;
        waiters[nw].flags = FUTEX2_SIZE_U32;
        waiters[nw].__reserved = 0;
        ++nw;
    }
    if (nw == 0) {
        // No attached readers AND no control entry — fall back to a
        // plain sleep so the caller doesn't burn a CPU spinning on
        // the empty list.
        struct timespec sleep_ts = {
            timeout_us / 1000000,
            (long)(timeout_us % 1000000) * 1000L,
        };
        nanosleep(&sleep_ts, NULL);
        goto done;
    }
    // Bump waiter_count on every reader we're about to park on so
    // writers know to FUTEX_WAKE us. Ordering matters: increment
    // first, then re-snapshot wake_seq inside futex_waitv (already
    // done above with memory_order_acquire). If a writer races us
    // and publishes between our wake_seq snapshot and futex_waitv,
    // the kernel returns EAGAIN — we re-drain in pass 3.
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r || !r->base) continue;
        atomic_fetch_add_explicit(&r->hdr->waiter_count, 1u,
                                  memory_order_release);
    }
    (void)futex_waitv_until(waiters, nw, timeout_us);
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r || !r->base) continue;
        atomic_fetch_sub_explicit(&r->hdr->waiter_count, 1u,
                                  memory_order_release);
    }

    // Pass 3: post-wake drain. Whatever woke us probably has data; the
    // other channels' subsequent publishes will catch the next call.
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r || !r->base) continue;
        for (;;) {
            size_t out_len = 0;
            uint8_t* buf = scratch;
            size_t buf_sz = scratch_sz;
            if (r->hdr->max_payload > scratch_sz) {
                if (r->hdr->max_payload > big_sz) {
                    free(big);
                    big_sz = r->hdr->max_payload;
                    big = (uint8_t*)malloc(big_sz);
                    if (!big) { delivered = -1; goto done; }
                }
                buf = big;
                buf_sz = big_sz;
            }
            int rc = rrb_reader_recv(r, buf, buf_sz, &out_len);
            if (rc != 0) break;
            if (cb(i, buf, out_len, user) != 0) { goto done; }
            ++delivered;
        }
    }
done:
    free(big);
    return delivered;
}

void rrb_reader_close(rrb_reader_t* r) {
    if (!r) return;
    if (r->base && r->base != MAP_FAILED) munmap(r->base, r->size);
    if (r->fd >= 0) close(r->fd);
    free(r);
}
