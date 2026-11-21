// Regression tests for the robustness of the raccoon_ring reader and writer:
//
//   1. A ring file is never shrunk while readers may have it mapped. A reader
//      that touches a page beyond a shrunk end of file gets SIGBUS.
//   2. A reader refuses a ring whose header describes more slots than the file
//      holds, instead of mapping it and crashing on the first slot access.
//   3. A frame larger than the receive buffer is reported (RRB_RECV_TOO_BIG),
//      not delivered cut off, and it can be skipped.
//   4. A reader keeps working while a writer re-creates the ring with changing
//      geometry.

#include "raccoon/raccoon_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace
{
    int g_failures = 0;
    int g_checks = 0;

    void check(bool cond, const char* what)
    {
        ++g_checks;
        if (!cond)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what);
        }
    }

    std::string ringPath(const char* channel)
    {
        char path[512]{};
        rrb_channel_to_path(channel, path, sizeof(path));
        return path;
    }

    void removeRing(const char* channel) { (void)::unlink(ringPath(channel).c_str()); }

    off_t fileSize(const char* channel)
    {
        struct stat st{};
        if (::stat(ringPath(channel).c_str(), &st) != 0) return -1;
        return st.st_size;
    }

    void testRingFileIsNeverShrunk()
    {
        const char* CH = "test/ring_robust/never_shrunk";
        removeRing(CH);

        rrb_writer_t* big = rrb_writer_create(CH, 64, 4096);
        check(big != nullptr, "big writer created");
        const off_t bigSize = fileSize(CH);
        check(bigSize > 64 * 4096, "ring file holds the big geometry");

        rrb_reader_t* reader = rrb_reader_open(CH);
        uint8_t frame[100] = {1, 2, 3};
        check(rrb_writer_publish(big, frame, sizeof(frame)) == 0, "publish into big ring");
        rrb_writer_destroy(big);

        rrb_writer_t* small = rrb_writer_create(CH, 4, 256);
        check(small != nullptr, "small writer created");
        check(fileSize(CH) >= bigSize, "re-creating a ring with a smaller geometry keeps the file size");

        // The reader still has the big mapping; it must be able to use the ring.
        check(rrb_writer_publish(small, frame, 10) == 0, "publish into small ring");
        uint8_t buf[4096];
        size_t len = 0;
        bool got = false;
        for (int i = 0; i < 20 && !got; ++i)
        {
            int rc = rrb_reader_recv(reader, buf, sizeof(buf), &len);
            if (rc == 0 && len == 10) got = true;
        }
        check(got, "reader receives the frame of the re-created ring");

        rrb_writer_destroy(small);
        rrb_reader_close(reader);
        removeRing(CH);
    }

    void testReaderRefusesRingLargerThanFile()
    {
        const char* CH = "test/ring_robust/truncated_file";
        removeRing(CH);

        rrb_writer_t* w = rrb_writer_create(CH, 64, 2048);
        check(w != nullptr, "writer created");
        uint8_t frame[32] = {7};
        for (int i = 0; i < 70; ++i) rrb_writer_publish(w, frame, sizeof(frame));

        // Cut the file down to the header plus one slot from outside, as a
        // misbehaving writer would. The header still claims 64 slots.
        check(::truncate(ringPath(CH).c_str(), 256 + 16 + 2048) == 0, "file truncated");

        rrb_reader_t* r = rrb_reader_open(CH);
        check(r != nullptr, "reader opens");
        uint8_t buf[4096];
        size_t len = 0;
        for (int i = 0; i < 10; ++i)
        {
            // Must neither crash (SIGBUS) nor deliver data from the missing pages.
            int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
            check(rc == 1, "reader treats a ring larger than its file as not ready");
        }
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        removeRing(CH);
    }

    void testOversizedFrameIsReportedAndSkippable()
    {
        const char* CH = "test/ring_robust/oversized";
        removeRing(CH);

        rrb_writer_t* w = rrb_writer_create(CH, 8, 2048);
        rrb_reader_t* r = rrb_reader_open(CH);
        check(w != nullptr && r != nullptr, "writer and reader created");

        std::vector<uint8_t> big(1000, 0xAB);
        uint8_t small[10];
        std::memset(small, 0xCD, sizeof(small));
        check(rrb_writer_publish(w, big.data(), big.size()) == 0, "publish big frame");
        check(rrb_writer_publish(w, small, sizeof(small)) == 0, "publish small frame");

        uint8_t buf[100];
        size_t len = 0;
        int rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
        check(rc == RRB_RECV_TOO_BIG, "a frame larger than the buffer reports RRB_RECV_TOO_BIG");
        check(len == big.size(), "the required size is reported");

        // The frame is not consumed: a bigger buffer gets it.
        std::vector<uint8_t> roomy(2048);
        rc = rrb_reader_recv(r, roomy.data(), roomy.size(), &len);
        check(rc == 0 && len == big.size(), "the same frame is delivered to a big enough buffer");

        // The small frame published after the big one is still queued.
        rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
        check(rc == 0 && len == sizeof(small), "the small frame after the big one is delivered");

        // Skipping: publish another big frame, skip it, get the following small one.
        rrb_writer_publish(w, big.data(), big.size());
        rrb_writer_publish(w, small, sizeof(small));
        rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
        check(rc == RRB_RECV_TOO_BIG, "second big frame reports RRB_RECV_TOO_BIG");
        check(rrb_reader_skip(r) == 0, "skip succeeds");
        rc = rrb_reader_recv(r, buf, sizeof(buf), &len);
        check(rc == 0 && len == sizeof(small), "after skipping, the next frame is delivered");
        check(buf[0] == 0xCD, "the delivered frame is the small one");

        rrb_reader_close(r);
        rrb_writer_destroy(w);
        removeRing(CH);
    }

    void testReaderSurvivesChangingGeometry()
    {
        const char* CH = "test/ring_robust/changing_geometry";
        removeRing(CH);

        std::atomic<bool> stop{false};
        std::atomic<uint64_t> received{0};
        std::thread reader([&] {
            rrb_reader_t* r = rrb_reader_open(CH);
            std::vector<uint8_t> buf(16384);
            size_t len = 0;
            while (!stop.load())
            {
                int rc = rrb_reader_recv_wait(r, buf.data(), buf.size(), &len, 500);
                if (rc == 0) ++received;
                else if (rc == RRB_RECV_TOO_BIG) (void)rrb_reader_skip(r);
            }
            rrb_reader_close(r);
        });

        std::vector<uint8_t> frame(200, 0x11);
        for (int round = 0; round < 300; ++round)
        {
            const bool big = (round % 2) == 0;
            rrb_writer_t* w = rrb_writer_create(CH, big ? 64 : 4, big ? 8192 : 256);
            if (!w) { check(false, "writer re-created"); break; }
            for (int i = 0; i < 20; ++i) rrb_writer_publish(w, frame.data(), frame.size());
            rrb_writer_destroy(w);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stop.store(true);
        reader.join();

        check(received.load() > 0, "reader received frames while the geometry changed");
        removeRing(CH);
    }
}

int main()
{
    testRingFileIsNeverShrunk();
    testReaderRefusesRingLargerThanFile();
    testOversizedFrameIsReportedAndSkippable();
    testReaderSurvivesChangingGeometry();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
