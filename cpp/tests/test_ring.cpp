// Tests of the raccoon_ring C API (ring format v4).
//
//   1.  channel name to file name mapping, invalid names
//   2.  file and directory permissions
//   3.  a new reader starts after the latest frame; a retained reader gets the latest once
//   4.  the first opener creates the ring, a reader can open before any writer
//   5.  several writer processes: every frame arrives exactly once
//   6.  a second writer does not wipe the frames of the first
//   7.  a writer that claims a slot and dies does not wedge readers
//   8.  the file never shrinks while a reader has it mapped
//   9.  a header that claims more than the file holds is not trusted
//  10.  a frame larger than the receive buffer is reported, not cut off
//  11.  a reader follows a ring that is re-initialised with another geometry
//  12.  a reader parked in a futex wait is woken by a restarted writer
//  13.  a reader follows a ring file that is deleted and created again
//  14.  slot stride is a multiple of 64 bytes, publish rejects too big frames

#include "raccoon/raccoon_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    void require(bool ok, const std::string& msg)
    {
        if (!ok) throw std::runtime_error(msg);
    }

    std::string pathOf(const char* ch)
    {
        char path[512]{};
        require(rrb_channel_to_path(ch, path, sizeof(path)) == 0, "path");
        return path;
    }

    void cleanup(const char* ch) { (void)::unlink(pathOf(ch).c_str()); }

    off_t fileSize(const char* ch)
    {
        struct stat st{};
        if (::stat(pathOf(ch).c_str(), &st) != 0) return -1;
        return st.st_size;
    }

    struct Frame { uint32_t writer; uint32_t n; };

    void drain(rrb_reader_t* r, std::multiset<uint64_t>& got)
    {
        for (;;)
        {
            Frame f{};
            size_t len = 0;
            if (rrb_reader_recv(r, &f, sizeof(f), &len) != 0) break;
            if (len == sizeof(f)) got.insert((uint64_t(f.writer) << 32) | f.n);
        }
    }

    uint64_t key(uint32_t w, uint32_t n) { return (uint64_t(w) << 32) | n; }

    void publish(rrb_writer_t* w, uint32_t id, uint32_t n)
    {
        Frame f{id, n};
        require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish");
    }

    // 1.
    void testPathMapping()
    {
        char p[256];
        require(rrb_channel_to_path("hardware/motor/0/cmd/power", p, sizeof(p)) == 0, "valid name");
        require(std::string(p) == "/dev/shm/raccoon-rings/hardware.motor.0.cmd.power", "mapping");
        require(rrb_channel_to_path("a_b-C/9", p, sizeof(p)) == 0, "allowed characters");
        for (const char* bad : {"", "/a", "a/", "a//b", "a b", "a.b", "a\\b", "ä", "a:b", "../x"})
            require(rrb_channel_to_path(bad, p, sizeof(p)) != 0, std::string("accepted: ") + bad);
        require(rrb_channel_to_path(std::string(128, 'a').c_str(), p, sizeof(p)) != 0, "too long");
        require(rrb_channel_to_path(std::string(127, 'a').c_str(), p, sizeof(p)) == 0, "longest name");
        require(rrb_channel_to_path("abc", p, 10) != 0, "output buffer too small");
        require(rrb_writer_open("a b", 4, 64) == nullptr, "writer on invalid name");
        require(rrb_reader_open("a b", 4, 64, 0) == nullptr, "reader on invalid name");
    }

    // 2.
    void testPermissions()
    {
        const char* CH = "test/ring/permissions";
        cleanup(CH);
        const mode_t old = umask(077);  // a strict umask must not decide the rights
        rrb_writer_t* w = rrb_writer_open(CH, 4, 64);
        umask(old);
        require(w != nullptr, "open");
        struct stat st{};
        require(::stat(pathOf(CH).c_str(), &st) == 0, "stat file");
        require((st.st_mode & 0777) == 0666, "file mode is not 0666");
        require(::stat("/dev/shm/raccoon-rings", &st) == 0, "stat dir");
        require((st.st_mode & 01777) == 0777, "directory mode is not 0777 (no sticky bit)");
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 3.
    void testReaderStartsAfterLatest()
    {
        const char* CH = "test/ring/start";
        cleanup(CH);
        rrb_writer_t* w = rrb_writer_open(CH, 8, 64);
        require(w != nullptr, "writer");
        for (uint32_t i = 1; i <= 5; ++i) publish(w, 1, i);

        rrb_reader_t* plain = rrb_reader_open(CH, 8, 64, 0);
        rrb_reader_t* retained = rrb_reader_open(CH, 8, 64, RRB_FLAG_RETAINED);
        require(plain && retained, "readers");
        std::multiset<uint64_t> a, b;
        drain(plain, a);
        drain(retained, b);
        require(a.empty(), "a plain reader saw old frames");
        require(b.size() == 1 && b.count(key(1, 5)) == 1, "a retained reader must get exactly the latest frame");

        publish(w, 1, 6);
        drain(plain, a);
        drain(retained, b);
        require(a.size() == 1 && a.count(key(1, 6)) == 1, "plain reader misses the next frame");
        require(b.size() == 2 && b.count(key(1, 6)) == 1, "retained reader misses the next frame");
        rrb_reader_close(plain);
        rrb_reader_close(retained);
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 4.
    void testReaderCreatesRing()
    {
        const char* CH = "test/ring/reader_first";
        cleanup(CH);
        rrb_reader_t* r = rrb_reader_open(CH, 16, 100, 0);
        require(r != nullptr, "reader");
        require(fileSize(CH) == (off_t)rrb_ring_bytes(16, 100), "the reader did not create the ring file");
        require(rrb_reader_max_payload(r) == 100, "max_payload");
        std::set<int> unused;
        rrb_writer_t* w = rrb_writer_open(CH, 16, 100);
        require(w != nullptr, "writer");
        publish(w, 3, 1);
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.count(key(3, 1)) == 1, "frame of a writer that came after the reader");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 5.
    void testConcurrentWriters()
    {
        const char* CH = "test/ring/concurrent";
        cleanup(CH);
        constexpr int kWriters = 4, kBursts = 150, kPerBurst = 10;  // 40 frames per burst < 64 slots
        rrb_reader_t* r = rrb_reader_open(CH, 64, 64, 0);
        require(r != nullptr, "reader");
        int go[kWriters][2], done[kWriters][2];
        std::vector<pid_t> kids;
        for (int i = 0; i < kWriters; ++i)
        {
            require(pipe(go[i]) == 0 && pipe(done[i]) == 0, "pipe");
            pid_t pid = fork();
            require(pid >= 0, "fork");
            if (pid == 0)
            {
                for (int j = 0; j <= i; ++j) close(go[j][1]);
                rrb_writer_t* w = rrb_writer_open(CH, 64, 64);
                if (!w) _exit(2);
                char b;
                uint32_t seq = 0;
                while (read(go[i][0], &b, 1) == 1 && b == 'g')
                {
                    for (int k = 0; k < kPerBurst; ++k)
                    {
                        Frame f{uint32_t(i), seq++};
                        if (rrb_writer_publish(w, &f, sizeof(f)) != 0) _exit(3);
                    }
                    if (write(done[i][1], "d", 1) != 1) _exit(4);
                }
                rrb_writer_destroy(w);
                _exit(0);
            }
            kids.push_back(pid);
        }
        std::multiset<uint64_t> got;
        for (int b = 0; b < kBursts; ++b)
        {
            for (int i = 0; i < kWriters; ++i) require(write(go[i][1], "g", 1) == 1, "go");
            for (int i = 0; i < kWriters; ++i) { char c; require(read(done[i][0], &c, 1) == 1, "done"); }
            drain(r, got);
        }
        for (int i = 0; i < kWriters; ++i) close(go[i][1]);
        for (pid_t p : kids)
        {
            int st = 0;
            waitpid(p, &st, 0);
            require(WIFEXITED(st) && WEXITSTATUS(st) == 0, "writer process failed");
        }
        rrb_reader_close(r);
        const size_t want = size_t(kWriters) * kBursts * kPerBurst;
        require(got.size() == want, "lost or duplicated frames: got " + std::to_string(got.size()) +
                                    " of " + std::to_string(want));
        require(std::set<uint64_t>(got.begin(), got.end()).size() == want, "duplicate frames");
        cleanup(CH);
    }

    // 6.
    void testSecondWriterKeepsFrames()
    {
        const char* CH = "test/ring/second_writer";
        cleanup(CH);
        rrb_reader_t* r = rrb_reader_open(CH, 64, 64, 0);
        rrb_writer_t* a = rrb_writer_open(CH, 64, 64);
        publish(a, 1, 7);
        rrb_writer_t* b = rrb_writer_open(CH, 64, 64);  // attaches, must not wipe
        publish(b, 2, 9);
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.count(key(1, 7)) == 1, "frame of the first writer was wiped");
        require(got.count(key(2, 9)) == 1, "frame of the second writer missing");
        rrb_reader_close(r);
        rrb_writer_destroy(a);
        rrb_writer_destroy(b);
        cleanup(CH);
    }

    void poke64(const char* ch, long offset, uint64_t value)
    {
        FILE* fp = fopen(pathOf(ch).c_str(), "r+b");
        require(fp != nullptr, "open ring file");
        fseek(fp, offset, SEEK_SET);
        require(fwrite(&value, 8, 1, fp) == 1, "poke");
        fclose(fp);
    }

    // 7.
    void testDeadWriterDoesNotWedge()
    {
        const char* CH = "test/ring/dead_writer";
        cleanup(CH);
        rrb_reader_t* r = rrb_reader_open(CH, 64, 64, 0);
        rrb_writer_t* w = rrb_writer_open(CH, 64, 64);
        publish(w, 1, 1);
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.size() == 1, "first frame");

        // A writer that claimed ticket 2 and died before it filled the slot: publish
        // tickets 2 and 3, then clear the seq of ticket 2's slot in the file.
        publish(w, 1, 2);
        publish(w, 1, 3);
        const long stride = (long)rrb_slot_stride(64);
        poke64(CH, 256 + stride * 1, 0);

        std::multiset<uint64_t> after;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (after.empty() && std::chrono::steady_clock::now() < deadline)
        {
            drain(r, after);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(after.count(key(1, 3)) == 1, "reader stayed stuck behind an unwritten slot");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 8.
    void testFileNeverShrinks()
    {
        const char* CH = "test/ring/never_shrinks";
        cleanup(CH);
        rrb_writer_t* big = rrb_writer_open(CH, 64, 4096);
        require(big != nullptr, "big writer");
        rrb_reader_t* r = rrb_reader_open(CH, 64, 4096, 0);
        const off_t before = fileSize(CH);
        rrb_writer_t* small = rrb_writer_open(CH, 4, 256);  // another geometry: re-initialised
        require(small != nullptr, "small writer");
        require(fileSize(CH) == before, "the ring file shrank");
        // The reader still maps the whole file: touching its last page must not raise SIGBUS.
        publish(small, 1, 1);
        std::multiset<uint64_t> got;
        drain(r, got);  // follows the new geometry from the header
        require(got.count(key(1, 1)) == 1, "reader did not follow the smaller ring");
        rrb_reader_close(r);
        rrb_writer_destroy(small);
        rrb_writer_destroy(big);
        cleanup(CH);
    }

    // 9.
    void testBogusHeaderIsNotTrusted()
    {
        const char* CH = "test/ring/bogus_header";
        cleanup(CH);
        rrb_writer_t* w = rrb_writer_open(CH, 4, 64);
        require(w != nullptr, "writer");
        rrb_writer_destroy(w);
        // slot_count at offset 8: claim a million slots in a file that holds four.
        FILE* fp = fopen(pathOf(CH).c_str(), "r+b");
        fseek(fp, 8, SEEK_SET);
        uint32_t huge = 1000000;
        require(fwrite(&huge, 4, 1, fp) == 1, "poke");
        fclose(fp);
        rrb_reader_t* r = rrb_reader_open(CH, 4, 64, 0);  // must re-initialise, not crash
        require(r != nullptr, "reader");
        rrb_writer_t* w2 = rrb_writer_open(CH, 4, 64);
        publish(w2, 1, 1);
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.count(key(1, 1)) == 1, "ring unusable after a bogus header");
        rrb_reader_close(r);
        rrb_writer_destroy(w2);
        cleanup(CH);
    }

    // 10.
    void testTooBigFrame()
    {
        const char* CH = "test/ring/too_big";
        cleanup(CH);
        rrb_reader_t* r = rrb_reader_open(CH, 8, 1024, 0);
        rrb_writer_t* w = rrb_writer_open(CH, 8, 1024);
        std::vector<uint8_t> payload(1000);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = (uint8_t)(i * 7);
        require(rrb_writer_publish(w, payload.data(), payload.size()) == 0, "publish");
        uint8_t small[16];
        size_t len = 0;
        require(rrb_reader_recv(r, small, sizeof(small), &len) == RRB_RECV_TOO_BIG, "too big not reported");
        require(len == payload.size(), "needed size");
        require(rrb_reader_recv(r, small, sizeof(small), &len) == RRB_RECV_TOO_BIG, "frame must stay pending");
        std::vector<uint8_t> big(len);
        require(rrb_reader_recv(r, big.data(), big.size(), &len) == 0, "retry with a big buffer");
        require(big == payload, "payload cut off or corrupt");

        require(rrb_writer_publish(w, payload.data(), payload.size()) == 0, "publish 2");
        require(rrb_reader_recv(r, small, sizeof(small), &len) == RRB_RECV_TOO_BIG, "too big 2");
        require(rrb_reader_skip(r) == 0, "skip");
        require(rrb_reader_recv(r, small, sizeof(small), &len) == 1, "nothing after the skip");
        require(rrb_reader_skip(r) == 1, "nothing to skip");

        std::vector<uint8_t> oversize(1025);
        require(rrb_writer_publish(w, oversize.data(), oversize.size()) == RRB_PUBLISH_TOO_BIG,
                "publish accepted more than max_payload");
        require(rrb_writer_publish(w, nullptr, 4) == -1, "null data");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 11.
    void testGeometryChange()
    {
        const char* CH = "test/ring/geometry_change";
        cleanup(CH);
        rrb_reader_t* r = rrb_reader_open(CH, 64, 256, 0);
        rrb_writer_t* w = rrb_writer_open(CH, 64, 256);
        std::multiset<uint64_t> got;
        for (int round = 0; round < 20; ++round)
        {
            const bool big = round % 2 == 0;
            rrb_writer_destroy(w);
            w = rrb_writer_open(CH, big ? 64 : 4, big ? 8192 : 256);
            require(w != nullptr, "reopen");
            publish(w, 9, (uint32_t)round);
            drain(r, got);
        }
        for (int round = 0; round < 20; ++round)
            require(got.count(key(9, (uint32_t)round)) == 1, "frame " + std::to_string(round) + " lost");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 12.
    void testParkedReaderIsWokenByRestartedWriter()
    {
        const char* CH = "test/ring/restart_wake";
        cleanup(CH);
        rrb_writer_t* w = rrb_writer_open(CH, 8, 64);
        rrb_reader_t* r = rrb_reader_open(CH, 8, 64, 0);
        std::atomic<bool> parked{false};
        std::atomic<long long> waitedMs{-1};
        std::thread t([&] {
            Frame f{};
            size_t len = 0;
            parked = true;
            const auto t0 = std::chrono::steady_clock::now();
            int rc = rrb_reader_recv_wait(r, &f, sizeof(f), &len, 5'000'000);
            waitedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0).count();
            if (rc != 0) waitedMs = -2;
        });
        while (!parked) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));  // let it park
        rrb_writer_destroy(w);
        w = rrb_writer_open(CH, 8, 64);  // a program restarts and attaches to the same ring
        publish(w, 1, 1);
        t.join();
        require(waitedMs.load() >= 0, "reader did not get the frame");
        require(waitedMs.load() < 1500, "the restarted writer did not wake the parked reader (waited " +
                                            std::to_string(waitedMs.load()) + " ms)");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(CH);
    }

    // 13.
    void testReaderFollowsRecreatedFile()
    {
        const char* CH = "test/ring/recreated";
        cleanup(CH);
        rrb_writer_t* w = rrb_writer_open(CH, 8, 64);
        rrb_reader_t* r = rrb_reader_open(CH, 8, 64, 0);
        publish(w, 1, 1);
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.count(key(1, 1)) == 1, "first frame");

        cleanup(CH);  // someone clears /dev/shm
        rrb_writer_t* w2 = rrb_writer_open(CH, 8, 64);
        publish(w2, 2, 2);
        // A reader that is only polled with recv is told to check the file.
        require(rrb_reader_revalidate(r) == 0, "revalidate");
        drain(r, got);
        require(got.count(key(2, 2)) == 1, "reader stayed on the deleted file");

        // The waiting calls check by themselves (about once a second while idle).
        cleanup(CH);
        rrb_writer_t* w3 = rrb_writer_open(CH, 8, 64);
        rrb_reader_t* readers[1] = {r};
        auto cb = [](size_t, const void*, size_t, void* user) -> int { ++*static_cast<int*>(user); return 0; };
        int seen = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        bool delivered = false;
        publish(w3, 3, 3);
        while (std::chrono::steady_clock::now() < deadline && !delivered)
        {
            rrb_reader_recv_wait_many(readers, 1, cb, &seen, 100'000);
            if (seen > 0) { delivered = true; break; }
            publish(w3, 3, 4);
        }
        require(delivered, "a waiting reader did not follow the re-created file");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        rrb_writer_destroy(w2);
        rrb_writer_destroy(w3);
        cleanup(CH);
    }

    // 14.
    void testStride()
    {
        for (uint32_t p : {1u, 12u, 48u, 49u, 64u, 100u, 4096u, 1000000u})
        {
            const size_t s = rrb_slot_stride(p);
            require(s % 64 == 0, "stride is not a multiple of 64");
            require(s >= 16 + p && s < 16 + p + 64, "stride too big or too small");
        }
        require(rrb_slot_stride(12) == 64, "a 12-byte message takes one cache line");
        require(rrb_slot_stride(48) == 64, "48 + 16 = 64");
        require(rrb_slot_stride(49) == 128, "49 + 16 needs two cache lines");
        require(rrb_ring_bytes(4, 12) == 256 + 4 * 64, "ring bytes");
    }
}

int main()
{
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        {"path mapping", testPathMapping},
        {"permissions", testPermissions},
        {"reader starts after the latest frame", testReaderStartsAfterLatest},
        {"a reader can create the ring", testReaderCreatesRing},
        {"concurrent writer processes", testConcurrentWriters},
        {"a second writer keeps the frames", testSecondWriterKeepsFrames},
        {"a dead writer does not wedge", testDeadWriterDoesNotWedge},
        {"the file never shrinks", testFileNeverShrinks},
        {"a bogus header is not trusted", testBogusHeaderIsNotTrusted},
        {"too big frame", testTooBigFrame},
        {"geometry change", testGeometryChange},
        {"a restarted writer wakes a parked reader", testParkedReaderIsWokenByRestartedWriter},
        {"a re-created ring file", testReaderFollowsRecreatedFile},
        {"slot stride", testStride},
    };
    int failed = 0;
    for (const auto& c : cases)
    {
        try
        {
            c.fn();
            std::printf("PASS  %s\n", c.name);
        }
        catch (const std::exception& e)
        {
            ++failed;
            std::printf("FAIL  %s: %s\n", c.name, e.what());
        }
    }
    std::printf("%d of %zu cases failed\n", failed, sizeof(cases) / sizeof(cases[0]));
    return failed ? 1 : 0;
}
