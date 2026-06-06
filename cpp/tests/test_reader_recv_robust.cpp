// Regression tests for rrb_reader_recv() NULL-pointer crashes.
//
// Background: on 2026-06-02 the stm32-data-reader on the production Pi
// was crashing with SIGSEGV inside rrb_reader_recv (offset +0x884bc on
// AArch64, instruction `ldr w1, [x1, #8]` with x1=NULL — the slot_count
// load on r->hdr). Root cause: when reader_reopen_if_unlinked() detached
// the reader's mmap (because the ring file had been unlinked) and the
// follow-up reader_attach() failed (file gone or magic check mismatched
// during producer restart), r->hdr was left NULL. Subsequent r->hdr
// derefs in the same recv() call — and at the top of the next for-loop
// iteration — segfaulted.
//
// These tests exercise the recv() entry/lap-detection code paths with a
// NULL/missing/unlinked underlying ring file. They must complete without
// crashing.

#include "raccoon/raccoon_ring.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
    int g_failures = 0;
    int g_tests = 0;

    void check(bool cond, const char* what)
    {
        ++g_tests;
        if (!cond)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    }

    std::string ringPath(const char* channel)
    {
        char path[512]{};
        if (rrb_channel_to_path(channel, path, sizeof(path)) != 0)
        {
            throw std::runtime_error("channel_to_path failed");
        }
        return path;
    }

    void unlinkRing(const char* channel)
    {
        (void)::unlink(ringPath(channel).c_str());
    }

    void testReaderRecvOnLazyReader()
    {
        // Open a reader for a channel that has no producer. The reader
        // is in "lazy" state — r->base/r->hdr are NULL. recv() must
        // return 1 (no message) instead of crashing.
        const char* CH = "test/recv_robust/lazy";
        unlinkRing(CH);

        rrb_reader_t* r = rrb_reader_open(CH);
        check(r != nullptr, "reader_open succeeds even without producer");

        char buf[64];
        size_t len = 0;
        for (int i = 0; i < 100; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            check(rc == 1, "recv on lazy reader returns 1 (no msg)");
            check(len == 0, "recv on lazy reader produces no payload");
        }

        rrb_reader_close(r);
    }

    void testReaderRecvAfterUnlinkWithoutReplacement()
    {
        // Producer creates ring, publishes one message, reader receives,
        // producer is destroyed and file unlinked. Subsequent recv()
        // calls must not crash even though the file is gone.
        const char* CH = "test/recv_robust/unlinked";
        unlinkRing(CH);

        rrb_writer_t* w = rrb_writer_create(CH, 16, 256);
        check(w != nullptr, "writer_create succeeds");

        const char* payload = "hello";
        check(rrb_writer_publish(w, payload, std::strlen(payload)) == 0,
              "publish succeeds");

        rrb_reader_t* r = rrb_reader_open(CH);
        check(r != nullptr, "reader_open succeeds with live producer");

        // Pump until we drain everything.
        char buf[256];
        size_t len = 0;
        int seen = 0;
        for (int i = 0; i < 10; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            if (rc == 0) ++seen;
        }
        check(seen >= 1, "reader saw initial publish");

        // Destroy writer, then unlink the file behind the reader's back —
        // mirrors the failure mode where systemd / cleanup scripts wipe
        // /dev/shm/raccoon_ring_* mid-flight.
        rrb_writer_destroy(w);
        (void)::unlink(ringPath(CH).c_str());

        // Now hammer recv(). The reader's last_seen is non-zero and the
        // underlying file is gone. reader_reopen_if_unlinked() will try
        // to detach + reattach; reattach must fail with ENOENT.
        for (int i = 0; i < 200; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            // 1 (no msg) is the only correct outcome — must not crash,
            // must not return 0 (no real data is available).
            check(rc == 1, "recv after unlink-without-replacement returns 1");
            check(len == 0, "recv after unlink-without-replacement no payload");
        }

        rrb_reader_close(r);
    }

    void testReaderRecvWhenProducerRestartsDuringPoll()
    {
        // Producer publishes, reader sees N messages, then producer
        // destroys + recreates the ring (simulates a service restart),
        // and publishes new messages. Reader must transparently catch
        // up without crashing.
        const char* CH = "test/recv_robust/restart";
        unlinkRing(CH);

        rrb_writer_t* w1 = rrb_writer_create(CH, 16, 256);
        check(w1 != nullptr, "writer1 create succeeds");

        for (int i = 0; i < 5; ++i)
        {
            char m[32];
            std::snprintf(m, sizeof(m), "pre%d", i);
            check(rrb_writer_publish(w1, m, std::strlen(m)) == 0, "pre-publish");
        }

        rrb_reader_t* r = rrb_reader_open(CH);
        check(r != nullptr, "reader_open succeeds");

        // Drain pre-restart messages so r->last_seen advances.
        char buf[256];
        size_t len = 0;
        int preSeen = 0;
        for (int i = 0; i < 20; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            if (rc == 0) ++preSeen;
        }
        check(preSeen >= 5, "reader drained pre-restart messages");

        // "Restart" the producer: destroy + unlink + recreate.
        rrb_writer_destroy(w1);
        (void)::unlink(ringPath(CH).c_str());

        // Hit recv() while the file is gone — must not crash, just
        // return "no message" (rc==1).
        for (int i = 0; i < 50; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            check(rc == 1, "recv during producer-down window returns 1");
        }

        // Recreate producer (same channel, fresh ring).
        rrb_writer_t* w2 = rrb_writer_create(CH, 16, 256);
        check(w2 != nullptr, "writer2 create succeeds");

        for (int i = 0; i < 5; ++i)
        {
            char m[32];
            std::snprintf(m, sizeof(m), "post%d", i);
            check(rrb_writer_publish(w2, m, std::strlen(m)) == 0, "post-publish");
        }

        // Reader must transparently re-attach on next recv() and see
        // the post-restart messages.
        int postSeen = 0;
        for (int i = 0; i < 100; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            if (rc == 0) ++postSeen;
        }
        check(postSeen >= 5, "reader caught post-restart messages");

        rrb_writer_destroy(w2);
        rrb_reader_close(r);
    }

    void testReaderRecvWithStaleLastSeenAndMissingFile()
    {
        // Sharpest reproducer for the AArch64 +0x884bc crash. After the
        // reader has built up a non-zero last_seen, we yank the file
        // out from underneath it. The first recv() that loads
        // r->hdr->producer_seq saw the old (still-mmapped, now-deleted)
        // file. reader_reopen_if_unlinked() detaches and the reattach
        // fails with ENOENT — the original code then dereferenced a
        // NULL r->hdr in the lap-detection branch. With the fix in
        // place, recv() returns 1 instead.
        const char* CH = "test/recv_robust/stale_then_gone";
        unlinkRing(CH);

        rrb_writer_t* w = rrb_writer_create(CH, 8, 64);
        check(w != nullptr, "writer create");

        // Publish enough to advance r->last_seen well past 0.
        for (int i = 0; i < 32; ++i)
        {
            char m[16];
            std::snprintf(m, sizeof(m), "m%d", i);
            (void)rrb_writer_publish(w, m, std::strlen(m));
        }

        rrb_reader_t* r = rrb_reader_open(CH);
        char buf[64];
        size_t len = 0;
        for (int i = 0; i < 50; ++i)
        {
            (void)rrb_reader_recv(r, buf, sizeof(buf), &len);
        }

        // Yank file. The reader's mmap is still alive (anonymous-backed
        // tmpfs); future fstat will show nlink=0.
        (void)::unlink(ringPath(CH).c_str());

        // Hammer recv. Pre-fix this crashed with SIGSEGV in the lap-
        // detection branch (`r->hdr->slot_count` on NULL r->hdr).
        for (int i = 0; i < 500; ++i)
        {
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            check(rc == 1, "recv after yank returns 1, never crashes");
        }

        rrb_writer_destroy(w);
        rrb_reader_close(r);
    }
} // anonymous namespace

int main()
{
    try
    {
        testReaderRecvOnLazyReader();
        testReaderRecvAfterUnlinkWithoutReplacement();
        testReaderRecvWhenProducerRestartsDuringPoll();
        testReaderRecvWithStaleLastSeenAndMissingFile();
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "EXCEPTION: %s\n", e.what());
        return 2;
    }

    std::fprintf(stderr, "%d tests, %d failures\n", g_tests, g_failures);
    return g_failures == 0 ? 0 : 1;
}
