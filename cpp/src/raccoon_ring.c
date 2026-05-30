// raccoon_ring.c — implementation. See raccoon_ring.h for design rationale.

// Need _POSIX_C_SOURCE for ftruncate visibility under -std=c11.
#define _POSIX_C_SOURCE 200809L
#include "raccoon/raccoon_ring.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// ---- On-disk layout ----------------------------------------------------

#pragma pack(push, 8)
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t slot_size;        // sizeof(slot_hdr) + max_payload, rounded up
    uint32_t max_payload;
    uint32_t _reserved0;
    _Atomic uint64_t producer_seq;  // monotonic, slot_idx = (seq-1) % count
    char     channel_name[128];
    uint8_t  pad[96];  // align header to 256 bytes (32 + 128 + 96 = 256)
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
        h->_reserved0  = 0;
        atomic_store_explicit(&h->producer_seq, 0, memory_order_release);
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

// Try to attach. Sets the mmap fields if successful; returns 0 on
// success, -2 if file doesn't exist yet (lazy), -1 on hard error.
static int reader_attach(rrb_reader_t* r) {
    int fd = -1;
    void* base = NULL;
    size_t size = 0;
    int rc = shm_open_mmap(r->path, /*create=*/0, 0, 0, r->channel,
                           &base, &size, &fd);
    if (rc != 0) return rc;

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

    uint64_t producer = atomic_load_explicit(&r->hdr->producer_seq,
                                             memory_order_acquire);
    if (producer == r->last_seen) return 1;

    // Producer restarted: the writer resets producer_seq back to 0
    // (rrb_writer_create re-initialises the header in-place every time
    // the producer process starts). When a subscriber's last_seen was
    // 5000 from before the restart and producer is now 1, we'd loop
    // forever picking "future" seq numbers that never come. Detect
    // it as producer_seq < last_seen and resync to one behind the
    // current producer so the very next read returns the latest frame.
    if (producer < r->last_seen) {
        r->last_seen = producer > 0 ? producer - 1 : 0;
    }

    // If we're behind by more than (slot_count - 1), the producer has
    // lapped us. Jump to the most recent slot we can still safely read,
    // skipping the lost frames.
    uint64_t next;
    if (producer - r->last_seen > (uint64_t)r->hdr->slot_count - 1u) {
        // Aim for one full lap behind the producer so we read the OLDEST
        // still-intact slot in the ring instead of the one the writer is
        // currently overwriting.
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
            // looked. Advance to that seq and re-poll.
            r->last_seen = seq_before - 1;
            return rrb_reader_recv(r, buf, buf_size, out_len);
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
    // Three retries failed (producer is writing faster than we can copy
    // this single slot). Skip and let the next poll catch up.
    r->last_seen = next;
    return 1;
}

void rrb_reader_close(rrb_reader_t* r) {
    if (!r) return;
    if (r->base && r->base != MAP_FAILED) munmap(r->base, r->size);
    if (r->fd >= 0) close(r->fd);
    free(r);
}
