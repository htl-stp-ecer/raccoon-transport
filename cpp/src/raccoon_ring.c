// raccoon_ring.c — implementation. See raccoon_ring.h for the design.

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
#include <sched.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// A reader that stops waiting gives its waiter_count back, but never below zero: a count
// that was reset while it waited would otherwise wrap to 0xFFFFFFFF, and the next wait
// would bring it to 0 again while a reader is parked, which hides that reader from writers.
static void waiter_leave(_Atomic uint32_t* count) {
    uint32_t cur = atomic_load_explicit(count, memory_order_relaxed);
    while (cur > 0 &&
           !atomic_compare_exchange_weak_explicit(count, &cur, cur - 1,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
    }
}

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

// Set once the kernel reports ENOSYS for futex_waitv; multi-channel waits then
// poll instead of parking (see rrb_reader_recv_wait_many_with_control).
static _Atomic int g_waitv_unsupported = 0;

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
    uint32_t slot_size;        // stride of one slot in bytes, a multiple of 64
    uint32_t max_payload;
    // Futex word the writers bump after every publish; readers park on it.
    _Atomic uint32_t wake_seq;
    // Readers increment it around their futex wait; a writer skips the FUTEX_WAKE
    // syscall while it is 0.
    _Atomic uint32_t waiter_count;
    uint32_t _pad0;                 // aligns producer_seq to 8 bytes
    _Atomic uint64_t producer_seq;  // highest published ticket; slot = (ticket-1) % count
    char     channel_name[128];     // for diagnostics
    uint32_t flags;                 // RRB_FLAG_*
    uint32_t _pad1;
    _Atomic uint64_t claim_seq;     // last ticket handed out to a writer (fetch_add)
    uint8_t  pad[72];               // 40 + 128 + 4 + 4 + 8 + 72 = 256
} ring_hdr_t;

typedef struct {
    _Atomic uint64_t seq;      // 0 = being written / empty; else the ticket that filled the slot
    uint32_t len;              // payload bytes valid
    uint32_t _reserved;
    uint8_t  data[];           // max_payload bytes
} slot_hdr_t;
#pragma pack(pop)

_Static_assert(sizeof(ring_hdr_t) == 256, "header must be 256 bytes");
_Static_assert(sizeof(slot_hdr_t) == 16,  "slot header must be 16 bytes");

#define SLOT_ALIGN 64u

size_t rrb_slot_stride(uint32_t max_payload) {
    return ((sizeof(slot_hdr_t) + max_payload + SLOT_ALIGN - 1) / SLOT_ALIGN) * SLOT_ALIGN;
}

size_t rrb_ring_bytes(uint32_t slot_count, uint32_t max_payload) {
    return sizeof(ring_hdr_t) + (size_t)slot_count * rrb_slot_stride(max_payload);
}

// A ring geometry is usable when it is self-consistent and the file holds every
// slot it describes. Readers check this before they index into the mapping:
// a header that claims more slots than the file has would make the first slot
// access hit a page beyond the end of the file (SIGBUS).
static int geometry_fits(uint32_t slot_count, uint32_t slot_size,
                         uint32_t max_payload, size_t file_bytes) {
    if (slot_count == 0 || max_payload == 0) return 0;
    if (slot_size != (uint32_t)rrb_slot_stride(max_payload)) return 0;
    return file_bytes >= rrb_ring_bytes(slot_count, max_payload);
}

// ---- Path ---------------------------------------------------------------

// A channel name is 0-9 a-z A-Z _ - and '/' between non-empty segments. In the
// file name every '/' is a '.', which cannot occur in a name, so the mapping is
// unambiguous and the directory stays flat.
int rrb_channel_to_path(const char* channel, char* out_path, size_t out_size) {
    if (!channel || !out_path) return -1;
    const size_t name_len = strlen(channel);
    if (name_len == 0 || name_len > RRB_MAX_CHANNEL_NAME) return -1;
    const size_t dir_len = strlen(RRB_DIR);
    if (dir_len + 1 + name_len + 1 > out_size) return -1;
    memcpy(out_path, RRB_DIR, dir_len);
    size_t op = dir_len;
    out_path[op++] = '/';
    char prev = '/';  // a name must not start with '/'
    for (const char* c = channel; *c; ++c) {
        const char ch = *c;
        if (ch == '/') {
            if (prev == '/') return -1;  // empty segment
            out_path[op++] = '.';
        } else if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                   (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
            out_path[op++] = ch;
        } else {
            return -1;
        }
        prev = ch;
    }
    if (prev == '/') return -1;  // a name must not end with '/'
    out_path[op] = '\0';
    return 0;
}

// Create the ring directory if needed. It is world-writable (no sticky bit) so
// that every user can create and re-create ring files.
static void ensure_ring_dir(void) {
    if (mkdir(RRB_DIR, 0777) == 0) (void)chmod(RRB_DIR, 0777);  // not subject to the umask
}

// ---- Open or create a ring ---------------------------------------------

// Opens the ring file of `path` under an exclusive flock and returns its mapping.
//  * A valid ring (magic, version, geometry that fits the file) is attached as-is.
//    For a writer (`writer` = 1) the geometry must also equal the wanted one;
//    a reader takes whatever geometry the ring has.
//  * Otherwise the ring is (re-)initialised with the wanted geometry. The file only
//    ever grows: readers may have a larger ring mapped, and a page beyond a
//    shortened end of file raises SIGBUS in them.
// Returns 0 on success (`*out_fd` stays open for the caller), -1 on failure.
static int ring_open(const char* path, const char* channel,
                     uint32_t want_slots, uint32_t want_payload,
                     uint32_t want_flags, int writer,
                     void** out_base, size_t* out_size, int* out_fd) {
    if (want_slots == 0 || want_payload == 0) return -1;
    ensure_ring_dir();

    int fd = -1;
    for (int attempt = 0; attempt < 3; ++attempt) {
        fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
        if (fd < 0) return -1;
        (void)fchmod(fd, 0666);  // not subject to the umask
        if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
        struct stat st;
        if (fstat(fd, &st) != 0) { close(fd); return -1; }
        if (st.st_nlink > 0) break;  // not deleted while we waited for the lock
        close(fd);
        fd = -1;
    }
    if (fd < 0) return -1;

    struct stat st;
    if (fstat(fd, &st) != 0) goto fail;
    size_t file_bytes = (size_t)st.st_size;

    if (file_bytes >= sizeof(ring_hdr_t)) {
        void* base = mmap(NULL, file_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base != MAP_FAILED) {
            ring_hdr_t* h = (ring_hdr_t*)base;
            const int valid =
                h->magic == RRB_MAGIC && h->version == RRB_VERSION &&
                geometry_fits(h->slot_count, h->slot_size, h->max_payload, file_bytes);
            const int same =
                h->slot_count == want_slots && h->max_payload == want_payload;
            if (valid && (!writer || same)) {
                *out_base = base;
                *out_size = file_bytes;
                *out_fd = fd;
                (void)flock(fd, LOCK_UN);
                return 0;
            }
            munmap(base, file_bytes);
        }
    }

    {
        size_t want_bytes = rrb_ring_bytes(want_slots, want_payload);
        if (file_bytes < want_bytes) {
            if (ftruncate(fd, (off_t)want_bytes) != 0) goto fail;
        } else {
            want_bytes = file_bytes;
        }
        void* base = mmap(NULL, want_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) goto fail;
        ring_hdr_t* h = (ring_hdr_t*)base;
        const uint32_t stride = (uint32_t)rrb_slot_stride(want_payload);
        // Parked readers and the writers share wake_seq and waiter_count; a ring that
        // already has this geometry keeps both. Resetting waiter_count would make the
        // writers skip the FUTEX_WAKE and a parked reader would wake only on timeout.
        const int same_layout =
            h->magic == RRB_MAGIC && h->version == RRB_VERSION &&
            h->slot_count == want_slots && h->slot_size == stride &&
            h->max_payload == want_payload;
        // Readers that attach while the header is rewritten see no magic and treat
        // the ring as not ready; the magic is stored last.
        __atomic_store_n(&h->magic, 0u, __ATOMIC_RELEASE);
        h->version     = RRB_VERSION;
        h->slot_count  = want_slots;
        h->slot_size   = stride;
        h->max_payload = want_payload;
        h->flags       = want_flags;
        atomic_store_explicit(&h->claim_seq, 0, memory_order_release);
        atomic_store_explicit(&h->producer_seq, 0, memory_order_release);
        if (!same_layout) {
            atomic_store_explicit(&h->wake_seq, 0u, memory_order_release);
            atomic_store_explicit(&h->waiter_count, 0u, memory_order_release);
        }
        memset(h->channel_name, 0, sizeof(h->channel_name));
        strncpy(h->channel_name, channel, sizeof(h->channel_name) - 1);
        uint8_t* slot_base = (uint8_t*)base + sizeof(ring_hdr_t);
        for (uint32_t i = 0; i < want_slots; ++i) {
            slot_hdr_t* s = (slot_hdr_t*)(slot_base + (size_t)i * stride);
            atomic_store_explicit(&s->seq, 0, memory_order_release);
            s->len = 0;
        }
        __atomic_store_n(&h->magic, RRB_MAGIC, __ATOMIC_RELEASE);
        *out_base = base;
        *out_size = want_bytes;
        *out_fd = fd;
        (void)flock(fd, LOCK_UN);
        return 0;
    }

fail:
    (void)flock(fd, LOCK_UN);
    close(fd);
    return -1;
}

// ---- Writer ------------------------------------------------------------

// The writer works with the geometry it opened the ring with, not with the shared
// header: whatever another process writes there, the writer stays inside the file
// it has mapped.
struct rrb_writer_s {
    int         fd;
    void*       base;
    size_t      size;
    ring_hdr_t* hdr;
    uint8_t*    slots;       // points to slot 0
    size_t      stride;
    uint32_t    slot_count;
    uint32_t    max_payload;
};

rrb_writer_t* rrb_writer_open(const char* channel,
                              uint32_t slot_count,
                              uint32_t max_payload) {
    if (!channel || slot_count == 0 || max_payload == 0) return NULL;
    char path[256];
    if (rrb_channel_to_path(channel, path, sizeof(path)) != 0) return NULL;

    rrb_writer_t* w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    if (ring_open(path, channel, slot_count, max_payload, 0, /*writer=*/1,
                  &w->base, &w->size, &w->fd) != 0) {
        free(w);
        return NULL;
    }
    w->hdr         = (ring_hdr_t*)w->base;
    w->slots       = (uint8_t*)w->base + sizeof(ring_hdr_t);
    w->stride      = rrb_slot_stride(max_payload);
    w->slot_count  = slot_count;
    w->max_payload = max_payload;
    return w;
}

int rrb_writer_publish(rrb_writer_t* w, const void* data, size_t len) {
    if (!w || (!data && len > 0)) return -1;
    if (len > w->max_payload) return RRB_PUBLISH_TOO_BIG;

    // Take a private slot ticket, fill the slot, then advance producer_seq to the
    // highest published ticket (CAS-max: writers can finish out of order).
    const uint64_t claim = atomic_fetch_add_explicit(&w->hdr->claim_seq, 1,
                                                     memory_order_acq_rel) + 1;
    slot_hdr_t* s = (slot_hdr_t*)(w->slots +
        (size_t)((claim - 1) % w->slot_count) * w->stride);
    // Tag the slot as in progress: a reader that is copying it sees seq change and
    // drops the copy.
    atomic_store_explicit(&s->seq, 0, memory_order_release);
    s->len = (uint32_t)len;
    if (len) memcpy(s->data, data, len);
    atomic_store_explicit(&s->seq, claim, memory_order_release);

    uint64_t cur = atomic_load_explicit(&w->hdr->producer_seq, memory_order_relaxed);
    while (cur < claim &&
           !atomic_compare_exchange_weak_explicit(&w->hdr->producer_seq, &cur, claim,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
    }

    // Bump the futex word and wake the parked readers. A reader snapshots wake_seq,
    // re-checks producer_seq and waits with FUTEX_WAIT(expected = snapshot); a bump
    // between its snapshot and its wait makes the kernel return EAGAIN, so no wake
    // is lost. The FUTEX_WAKE syscall is skipped while nobody is parked.
    atomic_fetch_add_explicit(&w->hdr->wake_seq, 1u, memory_order_release);
    if (atomic_load_explicit(&w->hdr->waiter_count, memory_order_acquire) > 0u) {
        (void)futex_wake_all(&w->hdr->wake_seq);
    }
    return 0;
}

void rrb_writer_destroy(rrb_writer_t* w) {
    if (!w) return;
    if (w->base && w->base != MAP_FAILED) munmap(w->base, w->size);
    if (w->fd >= 0) close(w->fd);
    // The ring file stays: readers keep their mapping and the next writer
    // attaches to it.
    free(w);
}

// ---- Reader ------------------------------------------------------------

#define REVALIDATE_INTERVAL_NS 1000000000ull  /* 1 s */

struct rrb_reader_s {
    char        path[256];
    char        channel[RRB_MAX_CHANNEL_NAME + 1];
    uint32_t    want_slots;    // what the table says; used when the file has to be created
    uint32_t    want_payload;
    uint32_t    flags;         // RRB_FLAG_RETAINED
    int         fd;
    void*       base;
    size_t      size;
    ring_hdr_t* hdr;
    uint8_t*    slots;
    size_t      stride;
    uint32_t    slot_count;    // geometry of the attached ring, read once at attach
    uint32_t    max_payload;   // (the shared header can change under us; these cannot)
    uint64_t    too_big_seq;   // ticket of the frame last reported as RRB_RECV_TOO_BIG
    uint64_t    last_seen;     // last delivered ticket
    uint64_t    stall_seq;       // ticket we are waiting on to be written
    uint64_t    stall_since_ns;  // ...since when (monotonic)
    uint64_t    last_check_ns;   // last revalidation
};

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void reader_detach(rrb_reader_t* r) {
    if (r->base && r->base != MAP_FAILED) munmap(r->base, r->size);
    if (r->fd >= 0) close(r->fd);
    r->base        = NULL;
    r->size        = 0;
    r->hdr         = NULL;
    r->slots       = NULL;
    r->stride      = 0;
    r->slot_count  = 0;
    r->max_payload = 0;
    r->too_big_seq = 0;
    r->fd          = -1;
    r->last_seen   = 0;
    r->stall_seq   = 0;
    r->stall_since_ns = 0;
}

// Attach to the ring (creating it if it is missing). `fresh` = 1 starts after the
// latest frame (the latest frame itself for a retained channel); `fresh` = 0
// replays everything the ring still holds, which is what a reader wants after the
// ring was re-initialised under it.
// Returns 0 on success, -1 when the ring cannot be opened.
static int reader_attach(rrb_reader_t* r, int fresh) {
    void* base = NULL;
    size_t size = 0;
    int fd = -1;
    if (ring_open(r->path, r->channel, r->want_slots, r->want_payload, r->flags,
                  /*writer=*/0, &base, &size, &fd) != 0) {
        return -1;
    }
    ring_hdr_t* h = (ring_hdr_t*)base;
    // Read the geometry once; a writer may rewrite the shared header at any time.
    r->fd          = fd;
    r->base        = base;
    r->size        = size;
    r->hdr         = h;
    r->slots       = (uint8_t*)base + sizeof(ring_hdr_t);
    r->slot_count  = h->slot_count;
    r->stride      = h->slot_size;
    r->max_payload = h->max_payload;
    r->too_big_seq = 0;
    r->stall_seq   = 0;
    r->last_check_ns = now_ns();
    r->last_seen   = 0;
    if (fresh) {
        const uint64_t prod = atomic_load_explicit(&h->producer_seq, memory_order_acquire);
        r->last_seen = (prod > 0 && (r->flags & RRB_FLAG_RETAINED)) ? prod - 1 : prod;
    }
    return 0;
}

rrb_reader_t* rrb_reader_open(const char* channel,
                              uint32_t slot_count,
                              uint32_t max_payload,
                              uint32_t flags) {
    if (!channel || slot_count == 0 || max_payload == 0) return NULL;
    rrb_reader_t* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (rrb_channel_to_path(channel, r->path, sizeof(r->path)) != 0) {
        free(r);
        return NULL;
    }
    strncpy(r->channel, channel, sizeof(r->channel) - 1);
    r->want_slots   = slot_count;
    r->want_payload = max_payload;
    r->flags        = flags;
    r->fd           = -1;
    if (reader_attach(r, /*fresh=*/1) != 0) {
        free(r);
        return NULL;
    }
    return r;
}

int rrb_reader_revalidate(rrb_reader_t* r) {
    if (!r) return -1;
    if (!r->base) return reader_attach(r, /*fresh=*/0) == 0 ? 0 : -1;
    r->last_check_ns = now_ns();
    struct stat st;
    if (fstat(r->fd, &st) != 0 || st.st_nlink > 0) return 0;
    // The file was deleted. A new ring is a new epoch: replay what it holds.
    reader_detach(r);
    return reader_attach(r, /*fresh=*/0) == 0 ? 0 : -1;
}

int rrb_reader_recv(rrb_reader_t* r, void* buf, size_t buf_size, size_t* out_len) {
    if (!r || !buf || !out_len) return -1;
    *out_len = 0;

    for (int attempt = 0; attempt < 8; ++attempt) {
        if (!r->base || !r->hdr) {
            // A previous re-attach failed (the ring file could not be opened).
            if (reader_attach(r, /*fresh=*/0) != 0) return 1;
        }

        // The ring was re-initialised under us with another geometry: the cached
        // stride would index the new slots wrongly. Re-attach and replay.
        if (r->hdr->slot_size != r->stride ||
            r->hdr->slot_count != r->slot_count ||
            r->hdr->max_payload != r->max_payload) {
            reader_detach(r);
            if (reader_attach(r, /*fresh=*/0) != 0) return 1;
        }

        uint64_t producer = atomic_load_explicit(&r->hdr->producer_seq,
                                                 memory_order_acquire);
        if (producer == r->last_seen) return 1;

        // The ring was re-initialised in place (same geometry): the ticket counter
        // restarted. Replay the new epoch from its start.
        if (producer < r->last_seen) {
            r->last_seen = 0;
            if (producer == 0) return 1;
        }

        // Lapped: jump to the oldest slot that is still intact.
        uint64_t next;
        if (producer - r->last_seen > (uint64_t)r->slot_count - 1u) {
            next = producer - ((uint64_t)r->slot_count - 1u);
        } else {
            next = r->last_seen + 1;
        }

        slot_hdr_t* s = (slot_hdr_t*)(r->slots +
            (size_t)((next - 1) % r->slot_count) * r->stride);

        // Seqlock read: snapshot seq, copy the payload, check seq again. A changed
        // seq means a writer overwrote the slot during the copy.
        for (int retry = 0; retry < 3; ++retry) {
            uint64_t seq_before = atomic_load_explicit(&s->seq, memory_order_acquire);
            if (seq_before == 0 || seq_before < next) {
                // The ticket is claimed but the slot is not filled yet (a writer is
                // copying a few bytes). Give it a moment, then report "no frame"
                // WITHOUT moving past it; the next poll picks it up. Only if the same
                // slot stays unwritten for RRB_MP_STALL_NS (the writer died between
                // claim and publish) do we skip it, so the ring cannot wedge.
                for (int spin = 0; spin < 200 && (seq_before == 0 || seq_before < next); ++spin) {
                    sched_yield();
                    seq_before = atomic_load_explicit(&s->seq, memory_order_acquire);
                }
                if (seq_before == 0 || seq_before < next) {
                    const uint64_t now = now_ns();
                    if (r->stall_seq != next) {
                        r->stall_seq = next;
                        r->stall_since_ns = now;
                        return 1;
                    }
                    if (now - r->stall_since_ns < RRB_MP_STALL_NS) return 1;
                    r->last_seen = next;  // the writer is gone: skip the hole
                    return 1;
                }
            }
            if (seq_before > next) {
                // A newer ticket already overwrote the slot. Continue from there.
                r->last_seen = seq_before - 1;
                break;
            }
            // seq_before == next: the slot holds the frame. Copy the payload.
            uint32_t len = s->len;
            if (len > r->max_payload) len = r->max_payload;
            if (len > buf_size) {
                // Does not fit. Report the size it needs and leave the frame in
                // place (the caller retries with a bigger buffer or skips it).
                // Confirm first that the slot still holds the frame whose length
                // was read.
                if (atomic_load_explicit(&s->seq, memory_order_acquire) == next) {
                    *out_len = len;
                    r->too_big_seq = next;
                    return RRB_RECV_TOO_BIG;
                }
                continue;  // overwritten meanwhile: look again
            }
            memcpy(buf, s->data, len);

            uint64_t seq_after = atomic_load_explicit(&s->seq, memory_order_acquire);
            if (seq_after == next) {
                *out_len = len;
                r->last_seen = next;
                r->too_big_seq = 0;
                return 0;
            }
            // Overwritten during the copy: retry with what is there now.
        }
    }
    // The writers kept moving while we were inspecting the ring. The next poll
    // retries from the updated last_seen.
    return 1;
}

int rrb_reader_recv_wait(rrb_reader_t* r, void* buf, size_t buf_size,
                         size_t* out_len, int timeout_us) {
    if (!r || !buf || !out_len) return -1;

    // Fast path: no syscall when a frame is already there.
    int rc = rrb_reader_recv(r, buf, buf_size, out_len);
    if (rc != 1) return rc;          // 0 = got a frame, -1 = error
    if (timeout_us <= 0) return 1;   // the caller wanted a non-blocking call
    if (!r->base) {
        struct timespec lazy = {0, 1000000L};  // 1 ms
        nanosleep(&lazy, NULL);
        return rrb_reader_recv(r, buf, buf_size, out_len);
    }

    // Announce the wait BEFORE the snapshot of wake_seq, so a writer that checks
    // waiter_count either sees us (and wakes us) or has published before our
    // snapshot (and the futex_wait returns EAGAIN at once).
    atomic_fetch_add_explicit(&r->hdr->waiter_count, 1u, memory_order_release);
    uint32_t expected = atomic_load_explicit(&r->hdr->wake_seq, memory_order_acquire);
    (void)futex_wait_until(&r->hdr->wake_seq, expected, timeout_us);
    waiter_leave(&r->hdr->waiter_count);
    // EAGAIN, EINTR and ETIMEDOUT all fall through: poll again either way.
    rc = rrb_reader_recv(r, buf, buf_size, out_len);
    if (rc == 1 && now_ns() - r->last_check_ns >= REVALIDATE_INTERVAL_NS) {
        (void)rrb_reader_revalidate(r);
    }
    return rc;
}

int rrb_reader_recv_wait_many(rrb_reader_t* const* readers, size_t n,
                              rrb_multi_handler cb, void* user,
                              int timeout_us) {
    return rrb_reader_recv_wait_many_with_control(readers, n, NULL, 0,
                                                  cb, user, timeout_us);
}

// Hands every frame the reader has to `cb`. The receive buffer starts on the stack
// and grows (heap) to exactly what a frame needs. Returns 0, 1 when `cb` asked to
// stop, -1 on allocation failure.
static int drain_reader(rrb_reader_t* r, size_t index, rrb_multi_handler cb, void* user,
                        uint8_t* scratch, size_t scratch_sz,
                        uint8_t** big, size_t* big_sz, int* delivered) {
    for (;;) {
        size_t out_len = 0;
        uint8_t* buf = *big ? *big : scratch;
        size_t buf_sz = *big ? *big_sz : scratch_sz;
        int rc = rrb_reader_recv(r, buf, buf_sz, &out_len);
        if (rc == RRB_RECV_TOO_BIG) {
            // Grow to exactly what this frame needs, not to the ring's max_payload:
            // a ring may be sized for frames of many MiB while its frames are a few KiB.
            uint8_t* grown = (uint8_t*)realloc(*big, out_len);
            if (!grown) return -1;
            *big = grown;
            *big_sz = out_len;
            continue;
        }
        if (rc != 0) return 0;
        if (cb(index, buf, out_len, user) != 0) return 1;
        ++*delivered;
    }
}

int rrb_reader_recv_wait_many_with_control(rrb_reader_t* const* readers, size_t n,
                                           uint32_t* control,
                                           uint32_t control_expected,
                                           rrb_multi_handler cb, void* user,
                                           int timeout_us) {
    // 128 is the kernel's futex_waitv cap; the control word takes one entry.
    if (!readers || !cb || n == 0) return -1;
    const size_t max_channels = control ? 127 : 128;
    if (n > max_channels) return -1;

    uint8_t scratch[2048];
    uint8_t* big = NULL;
    size_t big_sz = 0;
    int delivered = 0;
    int rc;

    // Pass 1: drain every reader without a syscall.
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r) continue;
        rc = drain_reader(r, i, cb, user, scratch, sizeof(scratch), &big, &big_sz, &delivered);
        if (rc < 0) { delivered = -1; goto done; }
        if (rc > 0) goto done;
    }
    if (delivered > 0 || timeout_us == 0) goto done;

    // Pass 2: park on the wake_seq of every attached reader, plus the control word.
    struct rrb_futex_waitv waiters[128];
    size_t nw = 0;
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r || !r->base) continue;
        waiters[nw].val = atomic_load_explicit(&r->hdr->wake_seq, memory_order_acquire);
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
        // Nothing to park on: sleep so the caller does not spin on an empty list.
        struct timespec sleep_ts = {
            timeout_us / 1000000,
            (long)(timeout_us % 1000000) * 1000L,
        };
        nanosleep(&sleep_ts, NULL);
        goto after_wait;
    }
    if (atomic_load_explicit(&g_waitv_unsupported, memory_order_relaxed)) {
        // No futex_waitv (before Linux 5.16): poll every millisecond instead of
        // parking. Wakes are late by up to 1 ms. A change of the control word ends
        // the wait early.
        const int slice_us = timeout_us < 1000 ? timeout_us : 1000;
        if (!(control && *(volatile uint32_t*)control != control_expected)) {
            struct timespec poll_ts = {0, (long)slice_us * 1000L};
            nanosleep(&poll_ts, NULL);
        }
    } else {
        // Announce the wait on every ring we park on, then take the snapshot (done
        // above): a writer that publishes in between makes futex_waitv return EAGAIN.
        for (size_t i = 0; i < n; ++i) {
            rrb_reader_t* r = readers[i];
            if (!r || !r->base) continue;
            atomic_fetch_add_explicit(&r->hdr->waiter_count, 1u, memory_order_release);
        }
        if (futex_waitv_until(waiters, nw, timeout_us) < 0 && errno == ENOSYS) {
            atomic_store_explicit(&g_waitv_unsupported, 1, memory_order_relaxed);
        }
        for (size_t i = 0; i < n; ++i) {
            rrb_reader_t* r = readers[i];
            if (!r || !r->base) continue;
            waiter_leave(&r->hdr->waiter_count);
        }
    }

after_wait:
    // Pass 3: drain what woke us.
    for (size_t i = 0; i < n; ++i) {
        rrb_reader_t* r = readers[i];
        if (!r) continue;
        rc = drain_reader(r, i, cb, user, scratch, sizeof(scratch), &big, &big_sz, &delivered);
        if (rc < 0) { delivered = -1; goto done; }
        if (rc > 0) goto done;
    }
    if (delivered == 0) {
        // Idle: about once a second check that the ring files are still the ones on
        // disk (one fstat per reader, not on every poll).
        const uint64_t now = now_ns();
        for (size_t i = 0; i < n; ++i) {
            rrb_reader_t* r = readers[i];
            if (r && (!r->base || now - r->last_check_ns >= REVALIDATE_INTERVAL_NS)) {
                (void)rrb_reader_revalidate(r);
            }
        }
    }
done:
    free(big);
    return delivered;
}

int rrb_reader_skip(rrb_reader_t* r) {
    if (!r) return 1;
    if (r->too_big_seq > r->last_seen) {
        r->last_seen = r->too_big_seq;
        r->too_big_seq = 0;
        return 0;
    }
    return 1;
}

size_t rrb_reader_max_payload(const rrb_reader_t* r) {
    return (r && r->base) ? (size_t)r->max_payload : 0;
}

void rrb_reader_close(rrb_reader_t* r) {
    if (!r) return;
    if (r->base && r->base != MAP_FAILED) munmap(r->base, r->size);
    if (r->fd >= 0) close(r->fd);
    free(r);
}
