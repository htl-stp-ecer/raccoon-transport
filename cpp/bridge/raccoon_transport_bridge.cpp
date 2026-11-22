// raccoon_transport_bridge.cpp — C interface of raccoon::Transport (see the header).

#include "raccoon_transport_bridge.h"

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "raccoon/Transport.h"

namespace
{
    constexpr size_t kMaxQueue = 64;

    struct Queue
    {
        std::mutex mtx;
        std::deque<std::vector<uint8_t>> frames;
    };

    struct Bridge
    {
        raccoon::Transport transport = raccoon::Transport::create();
        std::mutex mtx;  // guards queues
        std::unordered_map<uint64_t, std::shared_ptr<Queue>> queues;
    };
}

extern "C" {

int raccoon_transport_create(void** out_transport)
{
    if (!out_transport) return -1;
    try
    {
        auto* bridge = new Bridge();
        bridge->transport.start_background_spin();
        *out_transport = bridge;
        return 0;
    }
    catch (...)
    {
        return -2;
    }
}

void raccoon_transport_destroy(void* transport)
{
    if (!transport) return;
    auto* bridge = static_cast<Bridge*>(transport);
    bridge->transport.shutdown();  // joins the spin thread before the queues go
    delete bridge;
}

int raccoon_transport_publish(void* transport, const char* channel, const char* type_name,
                              const uint8_t* data, size_t len, int deduplicate)
{
    if (!transport || !channel || !data) return -1;
    auto* bridge = static_cast<Bridge*>(transport);
    raccoon::PublishOptions options;
    options.deduplicate = deduplicate != 0;
    return static_cast<int>(bridge->transport.publish_typed(channel, type_name, data, len, options));
}

int raccoon_transport_subscribe(void* transport, const char* channel, const char* type_name,
                                uint64_t* out_id)
{
    if (!transport || !channel || !out_id) return -1;
    auto* bridge = static_cast<Bridge*>(transport);
    auto queue = std::make_shared<Queue>();
    const auto id = bridge->transport.subscribe_typed(
        channel, type_name,
        [queue](const void* data, int len)
        {
            std::lock_guard<std::mutex> lk(queue->mtx);
            if (queue->frames.size() >= kMaxQueue) queue->frames.pop_front();
            const auto* bytes = static_cast<const uint8_t*>(data);
            queue->frames.emplace_back(bytes, bytes + len);
        });
    if (id == 0) return 1;
    {
        std::lock_guard<std::mutex> lk(bridge->mtx);
        bridge->queues.emplace(id, std::move(queue));
    }
    *out_id = id;
    return 0;
}

void raccoon_transport_unsubscribe(void* transport, uint64_t id)
{
    if (!transport) return;
    auto* bridge = static_cast<Bridge*>(transport);
    bridge->transport.unsubscribe(id);
    std::lock_guard<std::mutex> lk(bridge->mtx);
    bridge->queues.erase(id);
}

int raccoon_transport_receive(void* transport, uint64_t id, uint8_t* buf, size_t* out_len,
                              size_t max_len)
{
    if (!transport || !out_len) return -1;
    auto* bridge = static_cast<Bridge*>(transport);
    std::shared_ptr<Queue> queue;
    {
        std::lock_guard<std::mutex> lk(bridge->mtx);
        auto it = bridge->queues.find(id);
        if (it == bridge->queues.end()) return -1;
        queue = it->second;
    }
    std::lock_guard<std::mutex> lk(queue->mtx);
    if (queue->frames.empty()) return 1;
    const auto& frame = queue->frames.front();
    *out_len = frame.size();
    if (frame.size() > max_len || !buf) return 2;
    std::copy(frame.begin(), frame.end(), buf);
    queue->frames.pop_front();
    return 0;
}

const char* raccoon_transport_result_name(int code)
{
    if (code < 0) return "InvalidArguments";
    return raccoon::to_string(static_cast<raccoon::PublishResult>(code));
}

}  // extern "C"
