// The C interface of the transport (what Dart uses): publish, subscribe, receive, errors.

#include "raccoon_transport_bridge.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

namespace
{
    int g_failures = 0;
    void check(bool ok, const char* what)
    {
        if (!ok) { ++g_failures; std::fprintf(stderr, "FAIL: %s\n", what); }
    }

    void reset(const char* file) { (void)unlink((std::string("/dev/shm/raccoon-rings/") + file).c_str()); }

    std::vector<uint8_t> frame(int64_t ts, int32_t value)
    {
        std::vector<uint8_t> p(12);
        for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((ts >> (8 * (7 - i))) & 0xFF);
        for (int i = 0; i < 4; ++i) p[8 + i] = static_cast<uint8_t>((value >> (8 * (3 - i))) & 0xFF);
        return p;
    }

    bool receive(void* t, uint64_t id, std::vector<uint8_t>& out, int timeout_ms = 2000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        uint8_t buf[256];
        while (std::chrono::steady_clock::now() < deadline)
        {
            size_t len = 0;
            if (raccoon_transport_receive(t, id, buf, &len, sizeof(buf)) == 0)
            {
                out.assign(buf, buf + len);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
}

int main()
{
    reset("hardware.motor.0.cmd.power");
    void* t = nullptr;
    check(raccoon_transport_create(&t) == 0 && t, "create");

    uint64_t id = 0;
    check(raccoon_transport_subscribe(t, "hardware/motor/0/cmd/power", "scalar_i32_t", &id) == 0 && id != 0, "subscribe");
    uint64_t bad = 0;
    check(raccoon_transport_subscribe(t, "hardware/motor/0/cmd/power", "scalar_f_t", &bad) != 0, "wrong type refused");
    check(raccoon_transport_subscribe(t, "no/such/channel", nullptr, &bad) != 0, "unknown channel refused");

    auto a = frame(10, 50);
    check(raccoon_transport_publish(t, "hardware/motor/0/cmd/power", "scalar_i32_t", a.data(), a.size(), 0) == 0, "publish");
    std::vector<uint8_t> got;
    check(receive(t, id, got) && got == a, "frame arrives unchanged");

    check(raccoon_transport_publish(t, "hardware/motor/0/cmd/power", "scalar_f_t", a.data(), a.size(), 0) == 2, "type mismatch is 2");
    check(raccoon_transport_publish(t, "no/such/channel", nullptr, a.data(), a.size(), 0) == 1, "not in table is 1");
    check(raccoon_transport_publish(t, "hardware/motor/0/cmd/power", nullptr, a.data(), 5, 0) == 2, "wrong size is 2");
    check(std::string(raccoon_transport_result_name(2)) == "TypeMismatch", "result name");

    // A small buffer reports the needed size and keeps the frame.
    // (the channel is reliable: an identical frame would count as a re-send)
    auto b = frame(11, 51);
    check(raccoon_transport_publish(t, "hardware/motor/0/cmd/power", nullptr, b.data(), b.size(), 0) == 0, "publish 2");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint8_t tiny[4];
    size_t len = 0;
    check(raccoon_transport_receive(t, id, tiny, &len, sizeof(tiny)) == 2 && len == 12, "too small buffer");
    check(receive(t, id, got) && got == b, "frame is still queued");

    // The queue keeps the latest 64 frames.
    for (int i = 0; i < 100; ++i)
    {
        auto f = frame(1000 + i, i);
        raccoon_transport_publish(t, "hardware/motor/0/cmd/power", nullptr, f.data(), f.size(), 0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    int n = 0;
    uint8_t buf[64];
    while (raccoon_transport_receive(t, id, buf, &len, sizeof(buf)) == 0) ++n;
    check(n <= 64 && n > 0, "queue is bounded");

    raccoon_transport_unsubscribe(t, id);
    check(raccoon_transport_receive(t, id, buf, &len, sizeof(buf)) == -1, "unknown after unsubscribe");
    raccoon_transport_destroy(t);
    std::printf("%d failures\n", g_failures);
    return g_failures ? 1 : 0;
}
