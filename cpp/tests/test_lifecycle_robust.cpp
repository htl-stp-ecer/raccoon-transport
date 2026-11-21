// Regression tests for the lifecycle of raccoon::Transport:
//
//   1. stop() followed by destruction must not terminate the process when the
//      reliability thread was started.
//   2. shutdown() while another thread sits in spin_once() must not touch freed
//      readers.
//   3. subscribing while another thread dispatches frames must not race on the
//      handler list.
//   4. shutdown() called from inside a handler (on the spin thread) must not
//      deadlock or free state the spin loop still uses.
//   5. an exception that escapes a handler is reported, not swallowed silently.
//
// Run the executable under ThreadSanitizer / AddressSanitizer to see the data
// races and use-after-free errors; without a sanitizer the tests catch the
// terminate, the deadlock and the missing report.

#include "raccoon/Transport.h"
#include "raccoon/raccoon_ring.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace
{
    inline bool ok(raccoon::PublishResult r) { return r == raccoon::PublishResult::Ok; }
}

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

    void removeRing(const std::string& channel)
    {
        char path[512]{};
        if (rrb_channel_to_path(channel.c_str(), path, sizeof(path)) == 0) (void)::unlink(path);
    }

    std::vector<uint8_t> payload(int64_t ts, uint8_t value = 0)
    {
        std::vector<uint8_t> p(12, 0);  // the size of scalar_i32_t
        for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((ts >> (8 * (7 - i))) & 0xFF);
        p[8] = value;
        return p;
    }

    void testStopThenDestroyWithReliableThread()
    {
        const std::string CH = "hardware/motor/0/cmd/velocity";
        removeRing(CH);
        {
            raccoon::Transport t = raccoon::Transport::create();
            raccoon::PublishOptions options{};
            options.reliable = true;
            options.max_retries = 2;
            options.retry_interval = std::chrono::milliseconds(20);
            auto p = payload(1234567);
            check(ok(t.publish_raw(CH, p.data(), static_cast<int>(p.size()), options)),
                  "reliable publish succeeds and starts the reliability thread");
            t.stop();
            // Leaving the scope destroys a Transport whose reliability thread was
            // never joined; that used to call std::terminate.
        }
        check(true, "stop() followed by destruction returns normally");
        removeRing(CH);
    }

    void testShutdownWhileSpinning()
    {
        const std::string CH = "messages/warn";
        for (int round = 0; round < 60; ++round)
        {
            removeRing(CH);
            raccoon::Transport t = raccoon::Transport::create();
            std::atomic<int> got{0};
            t.subscribe_raw(CH, [&](const void*, int) { ++got; });

            std::thread spinner([&] {
                while (t.is_alive()) t.spin_once(200);
            });
            for (int i = 0; i < 5; ++i)
            {
                auto p = payload(1000 + i);
                ok(t.publish_raw(CH, p.data(), static_cast<int>(p.size())));
                std::this_thread::sleep_for(std::chrono::microseconds(300));
            }
            t.shutdown();
            spinner.join();
        }
        check(true, "shutdown() while a thread spins returns without crashing");
    }

    void testSubscribeWhileDispatching()
    {
        const std::string CH = "messages/error";
        removeRing(CH);
        raccoon::Transport t = raccoon::Transport::create();
        std::atomic<bool> stop{false};
        std::atomic<int> got{0};
        t.subscribe_raw(CH, [&](const void*, int) { ++got; });

        std::thread spinner([&] {
            while (!stop.load()) t.spin_once(100);
        });
        std::thread publisher([&] {
            int64_t ts = 5000;
            while (!stop.load())
            {
                auto p = payload(++ts);
                ok(t.publish_raw(CH, p.data(), static_cast<int>(p.size())));
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        });
        for (int i = 0; i < 200; ++i)
        {
            t.subscribe_raw(CH, [&](const void*, int) { ++got; });
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        stop.store(true);
        t.wake_control();
        publisher.join();
        spinner.join();
        check(got.load() > 0, "handlers added while frames flow still receive frames");
    }

    void testShutdownFromHandler()
    {
        const std::string CH = "camera/cmd/request";
        removeRing(CH);
        std::atomic<bool> done{false};
        {
            raccoon::Transport t = raccoon::Transport::create();
            t.subscribe_raw(CH, [&](const void*, int) {
                t.shutdown();  // on the spin thread itself
                done = true;
            });
            auto p = payload(777);
            ok(t.publish_raw(CH, p.data(), static_cast<int>(p.size())));

            std::thread spinner([&] {
                for (int i = 0; i < 50 && !done.load(); ++i) t.spin_once(100);
            });
            spinner.join();
            check(done.load(), "shutdown() from a handler returns (no self-deadlock)");
            check(!t.is_alive(), "transport is no longer alive after shutdown() from a handler");
        }
        check(true, "destruction after shutdown() from a handler returns normally");
    }

    void testHandlerExceptionIsReported()
    {
        const std::string CH = "camera/response";
        removeRing(CH);
        raccoon::Transport t = raccoon::Transport::create();
        std::atomic<int> reports{0};
        t.set_handler_error_handler([&](const std::string& channel, const std::string& what) {
            if (channel == CH && what.find("boom") != std::string::npos) ++reports;
        });
        t.subscribe_raw(CH, [](const void*, int) { throw std::runtime_error("boom"); });
        auto p = payload(4242);
        ok(t.publish_raw(CH, p.data(), static_cast<int>(p.size())));
        for (int i = 0; i < 20 && reports.load() == 0; ++i) t.spin_once(50);
        check(reports.load() > 0, "an exception from a handler reaches the handler error callback");
    }
}

int main()
{
    testStopThenDestroyWithReliableThread();
    testShutdownWhileSpinning();
    testSubscribeWhileDispatching();
    testShutdownFromHandler();
    testHandlerExceptionIsReported();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
