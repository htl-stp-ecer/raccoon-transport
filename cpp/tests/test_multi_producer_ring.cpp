// Multi-producer ring (rrb_writer_create_multi), the mode the reliable-delivery
// ACK channel uses because every subscriber process acks into the same ring.
//
//   1. several processes publish concurrently; the reader sees every frame once
//   2. a late writer attaching does NOT wipe frames already in the ring
//   3. a writer that claims a slot and dies does not wedge the reader
//   4. ACKs from several subscriber processes all reach one publisher
//      (the real topology that used to lose acks)

#include "raccoon/Channels.h"
#include "raccoon/Transport.h"
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

#include <sys/wait.h>
#include <unistd.h>

namespace
{
    constexpr const char* kCh = "test/multi_producer_ring";

    void require(bool ok, const std::string& msg)
    {
        if (!ok) throw std::runtime_error(msg);
    }

    void cleanup(const char* ch)
    {
        char path[512]{};
        if (rrb_channel_to_path(ch, path, sizeof(path)) == 0) (void)unlink(path);
    }

    struct Frame { uint32_t writer; uint32_t n; };

    // Drain everything currently readable.
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

    // 1. N writer processes, bursts small enough never to lap a 64-slot ring.
    void testConcurrentWriters()
    {
        cleanup(kCh);
        constexpr int kWriters = 4, kBursts = 150, kPerBurst = 10;  // 40 frames/burst < 64
        // Pipes as a barrier: each burst all writers publish at once.
        int go[kWriters][2], done[kWriters][2];
        std::vector<pid_t> kids;
        for (int i = 0; i < kWriters; ++i)
        {
            require(pipe(go[i]) == 0 && pipe(done[i]) == 0, "pipe");
            pid_t pid = fork();
            require(pid >= 0, "fork");
            if (pid == 0)
            {
                // Close the write ends inherited from earlier forks, otherwise a
                // sibling never sees EOF on its "go" pipe.
                for (int j = 0; j <= i; ++j) close(go[j][1]);
                rrb_writer_t* w = rrb_writer_create_multi(kCh, 64, 2048);
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
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        rrb_reader_t* r = rrb_reader_open(kCh);
        std::multiset<uint64_t> got;
        for (int b = 0; b < kBursts; ++b)
        {
            for (int i = 0; i < kWriters; ++i) require(write(go[i][1], "g", 1) == 1, "go");
            for (int i = 0; i < kWriters; ++i) { char c; require(read(done[i][0], &c, 1) == 1, "done"); }
            drain(r, got);
        }
        for (int i = 0; i < kWriters; ++i) close(go[i][1]);
        for (pid_t p : kids) { int st = 0; waitpid(p, &st, 0); require(WIFEXITED(st) && WEXITSTATUS(st) == 0, "writer failed"); }
        rrb_reader_close(r);

        const size_t want = size_t(kWriters) * kBursts * kPerBurst;
        require(got.size() == want, "lost or duplicated frames: got " + std::to_string(got.size()) +
                                    " of " + std::to_string(want));
        require(std::set<uint64_t>(got.begin(), got.end()).size() == want, "duplicate frames");
        cleanup(kCh);
    }

    // 2. A second writer attaching must keep the frames the first wrote.
    void testLateWriterDoesNotWipe()
    {
        cleanup(kCh);
        rrb_writer_t* a = rrb_writer_create_multi(kCh, 64, 2048);
        require(a != nullptr, "create a");
        Frame f{1, 7};
        require(rrb_writer_publish(a, &f, sizeof(f)) == 0, "publish a");
        rrb_writer_t* b = rrb_writer_create_multi(kCh, 64, 2048);  // would have wiped before
        require(b != nullptr, "create b");
        Frame g{2, 9};
        require(rrb_writer_publish(b, &g, sizeof(g)) == 0, "publish b");
        rrb_reader_t* r = rrb_reader_open(kCh);
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.count((uint64_t(1) << 32) | 7) == 1, "frame of the first writer was wiped");
        require(got.count((uint64_t(2) << 32) | 9) == 1, "frame of the second writer missing");
        rrb_reader_close(r);
        rrb_writer_destroy(a);
        rrb_writer_destroy(b);
        cleanup(kCh);
    }

    // 3. A claimed slot that is never filled must not block later frames.
    void testDeadWriterDoesNotWedge()
    {
        cleanup(kCh);
        rrb_writer_t* w = rrb_writer_create_multi(kCh, 64, 2048);
        require(w != nullptr, "create");
        rrb_reader_t* r = rrb_reader_open(kCh);
        Frame f{1, 1};
        require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish 1");
        std::multiset<uint64_t> got;
        drain(r, got);
        require(got.size() == 1, "first frame");

        // Simulate a writer that claimed ticket 2 and died before filling the slot:
        // publish tickets 2 and 3, then zero ticket 2's slot seq in the ring file.
        require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish 2");
        Frame g{1, 3};
        require(rrb_writer_publish(w, &g, sizeof(g)) == 0, "publish 3");
        char path[512]{};
        rrb_channel_to_path(kCh, path, sizeof(path));
        FILE* fp = fopen(path, "r+b");
        require(fp != nullptr, "open ring file");
        // slot 1 (ticket 2): header is 256 bytes, stride = round_up(16 + 2048, 8).
        long stride = 16 + 2048;
        fseek(fp, 256 + stride * 1, SEEK_SET);
        uint64_t zero = 0;
        require(fwrite(&zero, 8, 1, fp) == 1, "zero slot seq");
        fclose(fp);

        std::multiset<uint64_t> after;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (after.empty() && std::chrono::steady_clock::now() < deadline)
        {
            drain(r, after);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        require(after.count((uint64_t(1) << 32) | 3) == 1,
                "reader stayed stuck behind an unwritten slot");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(kCh);
    }


    // Header offsets in the ring file (see ring_hdr_t): producer_seq at 32, claim_seq at 176.
    void pokeU64(const char* ch, long offset, uint64_t value)
    {
        char path[512]{};
        rrb_channel_to_path(ch, path, sizeof(path));
        FILE* fp = fopen(path, "r+b");
        require(fp != nullptr, "open ring file");
        fseek(fp, offset, SEEK_SET);
        require(fwrite(&value, 8, 1, fp) == 1, "poke");
        fclose(fp);
    }

    std::multiset<uint64_t> readFor(rrb_reader_t* r, int ms)
    {
        std::multiset<uint64_t> got;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            drain(r, got);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return got;
    }

    // 5. The state found on a robot: producer_seq was left far above the tickets
    //    (754 vs 153), so no new ticket ever raised it and readers saw nothing new.
    void testStaleHighProducerSeqIsHealed()
    {
        cleanup(kCh);
        rrb_writer_t* w = rrb_writer_create_multi(kCh, 64, 2048);
        require(w != nullptr, "create");
        for (uint32_t n = 0; n < 3; ++n)
        {
            Frame f{1, n};
            require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish");
        }
        pokeU64(kCh, 32, 754);  // something else pushed producer_seq above claim_seq

        rrb_reader_t* r = rrb_reader_open(kCh);  // a reader that attaches now, like a new program
        for (uint32_t n = 100; n < 110; ++n)
        {
            Frame f{2, n};
            require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish after");
        }
        auto got = readFor(r, 500);
        for (uint32_t n = 100; n < 110; ++n)
            require(got.count((uint64_t(2) << 32) | n) == 1,
                    "frame " + std::to_string(n) + " is invisible behind a stale producer_seq");
        rrb_reader_close(r);
        rrb_writer_destroy(w);
        cleanup(kCh);
    }

    // 6. A ring reset in place by a single-producer writer (producer_seq back to 0,
    //    claim_seq and flag untouched) must not make readers wait for tickets that
    //    will never be written.
    void testRingResetByOldWriterDoesNotStallReaders()
    {
        cleanup(kCh);
        rrb_writer_t* w = rrb_writer_create_multi(kCh, 64, 2048);
        require(w != nullptr, "create");
        for (uint32_t n = 0; n < 150; ++n)
        {
            Frame f{1, n};
            require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish");
        }
        rrb_writer_t* old = rrb_writer_create(kCh, 64, 2048);  // what an old process does at start
        require(old != nullptr, "legacy create");
        rrb_reader_t* r = rrb_reader_open(kCh);
        for (uint32_t n = 200; n < 205; ++n)
        {
            Frame f{2, n};
            require(rrb_writer_publish(w, &f, sizeof(f)) == 0, "publish after reset");
        }
        auto got = readFor(r, 500);
        for (uint32_t n = 200; n < 205; ++n)
            require(got.count((uint64_t(2) << 32) | n) == 1,
                    "frame " + std::to_string(n) + " did not arrive within 500 ms after a reset");
        rrb_reader_close(r);
        rrb_writer_destroy(old);
        rrb_writer_destroy(w);
        cleanup(kCh);
    }

    // 4. The real topology: one publisher, several subscriber processes (each a
    //    separate ACK writer), lots of reliable commands. Nothing may need a
    //    retransmit and nothing may be given up on.
    void testManyAckersOnePublisher()
    {
        const std::string chans[3] = {"test/ack_a", "test/ack_b", "test/ack_c"};
        for (auto& c : chans) cleanup(c.c_str());
        cleanup(raccoon::Channels::Protocol::ACK);

        std::vector<pid_t> kids;
        for (int i = 0; i < 3; ++i)
        {
            pid_t pid = fork();
            require(pid >= 0, "fork");
            if (pid == 0)
            {
                raccoon::Transport rx = raccoon::Transport::create();
                rx.subscribeRaw(chans[i], [](const void*, int) {},
                                raccoon::SubscribeOptions{.reliable = true});
                auto end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
                while (std::chrono::steady_clock::now() < end) rx.spinOnce(20);
                rx.shutdown();
                _exit(0);
            }
            kids.push_back(pid);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        raccoon::Transport pub = raccoon::Transport::create();
        raccoon::PublishOptions po;
        po.reliable = true;
        po.retryInterval = std::chrono::milliseconds(50);
        po.maxRetries = 20;
        int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
        for (int round = 0; round < 40; ++round)
        {
            for (int i = 0; i < 3; ++i)
            {
                uint8_t p[12]{};
                ++ts;
                for (int b = 0; b < 8; ++b) p[b] = uint8_t((ts >> (8 * (7 - b))) & 0xFF);
                require(pub.publishRaw(chans[i], p, sizeof(p), po), "publish");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        auto st = pub.getAndResetStats();
        require(st.reliableDropped == 0, "a reliable command was given up on");
        require(st.reliableRetransmits == 0,
                "acks were lost: " + std::to_string(st.reliableRetransmits) + " retransmits");
        for (pid_t p : kids) waitpid(p, nullptr, 0);
        for (auto& c : chans) cleanup(c.c_str());
    }

    int run(const char* name, void (*fn)())
    {
        try { fn(); std::printf("PASS %s\n", name); return 0; }
        catch (const std::exception& e) { std::printf("FAIL %s: %s\n", name, e.what()); return 1; }
    }
}

int main()
{
    int failed = 0;
    failed += run("concurrent_writers", testConcurrentWriters);
    failed += run("late_writer_does_not_wipe", testLateWriterDoesNotWipe);
    failed += run("dead_writer_does_not_wedge", testDeadWriterDoesNotWedge);
    failed += run("stale_high_producer_seq_is_healed", testStaleHighProducerSeqIsHealed);
    failed += run("ring_reset_by_old_writer_does_not_stall_readers", testRingResetByOldWriterDoesNotStallReaders);
    failed += run("many_ackers_one_publisher", testManyAckersOnePublisher);
    return failed ? 1 : 0;
}
