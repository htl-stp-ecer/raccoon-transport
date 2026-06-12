// raccoon_ring_bridge.cpp — Dart FFI shim around raccoon_ring (the
// SHM ring buffer transport from raccoon-transport).
//
// History: this was originally a thin pure-C wrapper around the iceoryx2
// C API; then a C++ wrapper around raccoon::Transport (which itself
// wrapped iceoryx2); now a direct wrapper around raccoon_ring. Each
// step gained reliability — the iceoryx2 backend reproducibly broke
// new-node creation on the Pi once the reader had ~100 publishers open,
// and frames sporadically didn't reach subscribers. raccoon_ring is a
// fileless-per-channel SHM ring buffer: no daemon, no service
// descriptors, no state machine to wedge.

#include "raccoon_ring_bridge.h"
#include "raccoon/raccoon_ring.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

void log_line(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "raccoon_ring_bridge: %s\n", buf);
    fflush(stderr);
}

struct SubChannelState {
    rrb_reader_t* reader = nullptr;
    std::mutex mtx;
    std::deque<std::vector<uint8_t>> queue;
    static constexpr size_t kMaxQueue = 64;
    // First-frame-per-channel logging so we can tell from the journal
    // whether the bridge is actually getting data on each subscribed
    // channel. Quiet after the first frame.
    std::atomic<uint64_t> frames_received{0};
    std::string channel_name;
};

struct BridgeNode {
    std::atomic<bool> stop{false};

    // Channel name → shared subscriber state. Multiple BridgeSubscriber
    // handles on the same channel reuse the same state + queue (matching
    // the previous bridge's semantics) and share one underlying rrb_reader.
    std::mutex subs_mtx;
    std::unordered_map<std::string, std::shared_ptr<SubChannelState>> subs;
    // Snapshot of every subscriber's shared_ptr indexed by position in
    // the readers[] array we pass to rrb_reader_recv_wait_many. Rebuilt
    // by the poll loop whenever subs_changed becomes true.
    std::vector<std::shared_ptr<SubChannelState>> subs_snapshot;
    std::atomic<bool> subs_changed{false};

    // SHARED poll thread. Parks once in rrb_reader_recv_wait_many across
    // every subscribed channel via futex_waitv — wakes on ANY publish,
    // drains every channel that has data, re-parks. Beats N per-channel
    // threads (N × scratch buffer, N × kernel-side wait state, N ×
    // context switches per publish round).
    std::thread poll_thread;
    std::atomic<bool> stop_thread{false};
    std::atomic<bool> poll_started{false};

    // Per-channel publisher writers. Lazy-create on first publish, then
    // reused (raccoon_ring is single-producer per channel, so multiple
    // Dart BridgePublisher handles on the same channel must share
    // one rrb_writer).
    std::mutex pubs_mtx;
    std::unordered_map<std::string, rrb_writer_t*> pubs;
};

struct BridgePublisher {
    BridgeNode* node;
    std::string channel;
};

// Compute (slot_count, max_payload) for a ring sized to fit `size_hint`
// bytes with 2× headroom: ~128 KiB total memory budget, ≥4 slots. Matches
// the policy in raccoon::Transport::Impl::sizeRing so a bridge-published
// channel ends up with the same shape a Transport-published one would.
void ring_dims(size_t size_hint, size_t& slots, size_t& want) {
    want = (size_hint ? size_hint : 1) * 2;
    if (want < RRB_DEFAULT_MAX_PAYLOAD) want = RRB_DEFAULT_MAX_PAYLOAD;
    want = ((want + 255) / 256) * 256;
    slots = (128u * 1024u) / want;
    if (slots < 4) slots = 4;
    if (slots > RRB_DEFAULT_SLOT_COUNT) slots = RRB_DEFAULT_SLOT_COUNT;
}

struct BridgeSubscriber {
    BridgeNode* node;
    std::string channel;
    std::shared_ptr<SubChannelState> state;
};

// Multi-channel dispatcher. Called by rrb_reader_recv_wait_many for each
// frame it drains across the snapshot[] reader set. user_data is the
// BridgeNode so we can look the subscriber state up by index.
int sub_multi_handler(size_t reader_index, const void* payload,
                      size_t len, void* user) {
    auto* node = static_cast<BridgeNode*>(user);
    if (reader_index >= node->subs_snapshot.size()) return 0;
    auto& s = node->subs_snapshot[reader_index];
    if (!s) return 0;

    uint64_t prev = s->frames_received.fetch_add(1);
    if (prev == 0) {
        log_line("first frame on '%s' (%zu bytes)",
                 s->channel_name.c_str(), len);
    }
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        if (s->queue.size() >= SubChannelState::kMaxQueue) {
            s->queue.pop_front();
        }
        const auto* bytes = static_cast<const uint8_t*>(payload);
        s->queue.emplace_back(bytes, bytes + len);
    }
    return 0;
}

// SHARED poll loop. Snapshots the subscriber set once, then parks in
// futex_waitv across every reader's wake_seq. One thread per node
// instead of one per channel — the heavy savings on a Pi running ~20
// subscribed channels are fewer kernel wait records, fewer context
// switches per publish round, and one scratch buffer instead of N.
void node_poll_loop(BridgeNode* node) {
    std::vector<rrb_reader_t*> readers;
    readers.reserve(128);

    while (!node->stop_thread.load(std::memory_order_relaxed)) {
        // Cheap-load the changed flag and rebuild only when subscribers
        // were added (the common steady-state case is "no change since
        // the last loop", which skips the lock entirely).
        if (node->subs_changed.exchange(false,
                                        std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(node->subs_mtx);
            node->subs_snapshot.clear();
            readers.clear();
            for (auto& [_, s] : node->subs) {
                node->subs_snapshot.push_back(s);
                readers.push_back(s->reader);
            }
        }
        if (readers.empty()) {
            // No subscribers yet — sleep a bit so we don't hot-loop
            // before the first subscriber_create arrives.
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        // 50 ms timeout is just a heartbeat so stop_thread/subs_changed
        // are observed promptly even on a totally idle ring.
        (void)rrb_reader_recv_wait_many(readers.data(), readers.size(),
                                        sub_multi_handler, node,
                                        /*timeout_us=*/50 * 1000);
    }
}

} // namespace

extern "C" {

int raccoon_ring_bridge_node_create(void** out_node, const char* name) {
    if (!out_node || !name) return -1;
    try {
        auto* node = new BridgeNode();
        // No central poll thread any more — one is spawned per subscriber
        // inside raccoon_ring_bridge_subscriber_create so each channel can park
        // in its own futex_wait and wake independently in ~us.
        log_line("node_create('%s') ok (rrb backend, event-driven)", name);
        *out_node = node;
        return 0;
    } catch (const std::exception& e) {
        log_line("node_create('%s') threw: %s", name, e.what());
        return -2;
    } catch (...) {
        return -3;
    }
}

void raccoon_ring_bridge_node_destroy(void* n) {
    if (!n) return;
    auto* node = static_cast<BridgeNode*>(n);
    node->stop.store(true);
    node->stop_thread.store(true, std::memory_order_release);
    if (node->poll_thread.joinable()) node->poll_thread.join();
    {
        std::lock_guard<std::mutex> lk(node->subs_mtx);
        for (auto& [_, s] : node->subs) {
            if (s->reader) rrb_reader_close(s->reader);
        }
        node->subs.clear();
        node->subs_snapshot.clear();
    }
    {
        std::lock_guard<std::mutex> lk(node->pubs_mtx);
        for (auto& [_, w] : node->pubs) {
            if (w) rrb_writer_destroy(w);
        }
        node->pubs.clear();
    }
    delete node;
}

int raccoon_ring_bridge_publisher_create(void* n, const char* channel, void** out_pub) {
    if (!n || !channel || !out_pub) return -1;
    auto* node = static_cast<BridgeNode*>(n);
    try {
        auto* pub = new BridgePublisher{node, std::string(channel)};
        *out_pub = pub;
        return 0;
    } catch (...) {
        return -2;
    }
}

int raccoon_ring_bridge_publisher_send(void* p, const uint8_t* data, size_t len) {
    if (!p || !data || len == 0) return -1;
    auto* pub = static_cast<BridgePublisher*>(p);
    // The whole publish runs under pubs_mtx: rrb_writer is not thread-safe,
    // and the grow path below destroys + re-creates the writer — another
    // thread publishing on the same channel must never see the dangling
    // handle. Dart is single-isolate so there is no contention in practice.
    std::lock_guard<std::mutex> lk(pub->node->pubs_mtx);
    rrb_writer_t* w;
    auto it = pub->node->pubs.find(pub->channel);
    if (it == pub->node->pubs.end()) {
        size_t slots = 0, want = 0;
        ring_dims(len, slots, want);
        w = rrb_writer_create(pub->channel.c_str(),
                              (uint32_t)slots, (uint32_t)want);
        if (!w) {
            log_line("publisher_send: rrb_writer_create('%s') failed",
                     pub->channel.c_str());
            return -2;
        }
        pub->node->pubs[pub->channel] = w;
    } else {
        w = it->second;
    }
    int rc = rrb_writer_publish(w, data, len);
    if (rc != 0) {
        // Only failure path in rrb_writer_publish is "payload too big for
        // the ring's max_payload". Grow-and-republish: tear the writer
        // down and re-create it with a bigger ring (rrb_writer_create
        // wipes + re-inits the SHM file in place; readers detect the
        // slot_size change and re-attach). Mirrors
        // raccoon::Transport::publishRaw's resize path.
        log_line("publisher_send('%s') rejected %zu-byte payload — resizing ring",
                 pub->channel.c_str(), len);
        size_t slots = 0, want = 0;
        ring_dims(len, slots, want);
        rrb_writer_destroy(w);
        w = rrb_writer_create(pub->channel.c_str(),
                              (uint32_t)slots, (uint32_t)want);
        if (!w) {
            pub->node->pubs.erase(pub->channel);
            log_line("publisher_send: rrb_writer_create('%s') failed during grow",
                     pub->channel.c_str());
            return -2;
        }
        pub->node->pubs[pub->channel] = w;
        rc = rrb_writer_publish(w, data, len);
        if (rc != 0) {
            log_line("publisher_send('%s') still rejected %zu-byte payload after resize",
                     pub->channel.c_str(), len);
            return -3;
        }
    }
    return 0;
}

void raccoon_ring_bridge_publisher_destroy(void* p) {
    if (!p) return;
    delete static_cast<BridgePublisher*>(p);
    // The underlying rrb_writer stays alive — multiple Dart publishers
    // can share one writer, and the node teardown is what closes them.
}

int raccoon_ring_bridge_subscriber_create(void* n, const char* channel, void** out_sub) {
    if (!n || !channel || !out_sub) return -1;
    auto* node = static_cast<BridgeNode*>(n);
    try {
        std::shared_ptr<SubChannelState> state;
        bool fresh = false;
        {
            std::lock_guard<std::mutex> lk(node->subs_mtx);
            auto& slot = node->subs[channel];
            if (!slot) {
                slot = std::make_shared<SubChannelState>();
                slot->channel_name = channel;
                slot->reader = rrb_reader_open(channel);
                if (!slot->reader) {
                    node->subs.erase(channel);
                    log_line("subscriber_create('%s') — rrb_reader_open failed",
                             channel);
                    return -2;
                }
                fresh = true;
                log_line("subscriber_create('%s') ok", channel);
            }
            state = slot;
        }
        if (fresh) {
            // Signal the shared poll thread to rebuild its snapshot on
            // the next loop iteration. We don't notify-wake it — the
            // 50 ms futex_waitv heartbeat picks the change up within
            // one tick, which is fine for subscribe-time latency.
            node->subs_changed.store(true, std::memory_order_release);
            // Lazy-start the shared poll thread on first subscribe.
            bool expected = false;
            if (node->poll_started.compare_exchange_strong(expected, true)) {
                node->poll_thread = std::thread(node_poll_loop, node);
            }
        }
        auto* sub = new BridgeSubscriber{node, std::string(channel), state};
        *out_sub = sub;
        return 0;
    } catch (const std::exception& e) {
        log_line("subscriber_create('%s') threw: %s", channel, e.what());
        return -3;
    } catch (...) {
        return -4;
    }
}

int raccoon_ring_bridge_subscriber_receive(void* s, uint8_t* buf,
                                   size_t* out_len, size_t max_len) {
    if (!s || !buf || !out_len) return -1;
    auto* sub = static_cast<BridgeSubscriber*>(s);
    if (!sub->state) { *out_len = 0; return 1; }
    std::vector<uint8_t> frame;
    {
        std::lock_guard<std::mutex> lk(sub->state->mtx);
        if (sub->state->queue.empty()) {
            *out_len = 0;
            return 1;
        }
        frame = std::move(sub->state->queue.front());
        sub->state->queue.pop_front();
    }
    size_t n = frame.size();
    if (n > max_len) n = max_len;
    std::memcpy(buf, frame.data(), n);
    *out_len = n;
    return 0;
}

void raccoon_ring_bridge_subscriber_destroy(void* s) {
    if (!s) return;
    delete static_cast<BridgeSubscriber*>(s);
    // Same as publisher_destroy: SubChannelState stays alive until node
    // teardown so other Dart subscribers on the same channel keep getting
    // frames.
}

} // extern "C"
