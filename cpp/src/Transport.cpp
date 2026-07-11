// Transport.cpp — raccoon::Transport backed by the in-tree raccoon_ring
// SHM library.
//
// History: this used to wrap iceoryx2 v0.9.999-dev. That backend proved
// unreliable in our deployed setup (Pi with stm32-data-reader publishing
// ~100 sensor channels) — every new iceoryx2 process on the host would
// fail at NodeBuilder::create with InternalError, and even when subscribes
// nominally succeeded, frames did not always reach the subscriber. We
// replaced the backend with raccoon_ring: one /dev/shm file per channel,
// single-producer multi-consumer ring with a per-slot SeqLock. Same
// raccoon::Transport public API (publish/subscribe/spinOnce/...), no
// caller-side changes anywhere in the codebase.
//
// Threading model:
//   * one recursive mutex guards the per-channel writer/subscriber maps
//   * rrb_writer / rrb_reader instances themselves are NOT thread-safe;
//     all calls happen inside the mutex
//   * publishRaw is allowed from any thread (mutex serialises into the
//     single-producer ring writer — fine, raccoon_ring is documented as
//     SPMC; a serialised SPSC chain is equivalent)
//   * subscribeRaw registers a handler; spinOnce polls each subscriber's
//     ring under the mutex, releases the mutex around each handler call
//     so a slow handler does not block unrelated transport ops
//
// Latency stats are computed from the LCM message timestamp where present.
// raccoon_ring delivers raw bytes; we sniff the first 8 bytes as a big-
// endian int64 microseconds-since-epoch, matching every raccoon LCM type's
// `timestamp` field layout. If the message is shorter than 8 bytes we
// just don't record latency for it.

#include "raccoon/Transport.h"

#include "raccoon/Channels.h"
#include "raccoon/Dedup.h"
#include "raccoon/ack_t.hpp"
#include "raccoon/raccoon_ring.h"

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// The legacy reliable/retry/retain flags are kept on PublishOptions and
// SubscribeOptions for API compatibility but the SHM backend ignores them.
// Suppress the deprecation warning on our own pass-through reads.
#if defined(__GNUC__) || defined(__clang__)
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace raccoon
{
    namespace
    {
        constexpr int kSpinIdleSleepMs = 1;
        // How often the background reliability thread drains ACKs and
        // re-sends un-acked commands. Finer than the default 50 ms retry
        // interval so retries fire close to schedule.
        constexpr int kReliabilityTickMs = 5;
    }

    class Transport::Impl
    {
    public:
        // We carry an "alive" flag so shutdown() can be called explicitly
        // before the destructor (the Python atexit path needs this) and
        // subsequent publishes become safe no-ops.
        std::atomic<bool> running{true};

        struct PubEntry
        {
            rrb_writer_t* writer = nullptr;
            // Track the ring's current max_payload so publishRaw can
            // detect outgrowth and re-create the writer with a larger
            // ring instead of dropping the frame.
            uint32_t max_payload = 0;
            // Last value bytes published with deduplicate=true on this
            // channel — see raccoon::dedup::shouldDrop.
            dedup::LastPayload dedupState;
        };
        struct SubEntry
        {
            std::string channel;
            rrb_reader_t* reader = nullptr;
            std::vector<Transport::RawHandler> handlers;
            // Per-channel scratch buffer for the receive copy. Sized at
            // RRB_DEFAULT_MAX_PAYLOAD; if any channel ends up needing
            // larger we grow it lazily.
            std::vector<uint8_t> scratch;
        };

        std::unordered_map<std::string, PubEntry>   publishers;
        std::vector<std::unique_ptr<SubEntry>>      subscribers;

        // Coarse mutex protects every Impl mutation. Recursive so a
        // callback that publishes back does not deadlock.
        std::recursive_mutex apiMutex;

        // Pure event-driven spin loop: the spin thread parks on
        // futex_waitv across (a) every subscriber's wake_seq AND
        // (b) this control word. Subscribe / unsubscribe / shutdown
        // bump controlSeq + FUTEX_WAKE so the spin thread re-snapshots
        // its subscriber list (or notices the stop request) without any
        // periodic polling. Bumped via Transport::wakeControl().
        std::atomic<uint32_t> controlSeq{0};

        // Count of value-channel publishes dropped because their payload
        // was byte-identical to the previous one (deduplicate=true).
        // Surfaced via getAndResetStats().publishesDeduplicated.
        std::atomic<uint64_t> dedupCount{0};

        // ---- Stats (mostly unchanged from the iceoryx2 version) -------
        struct ChannelStatsAccumulator
        {
            uint64_t deliveries{0};
            int64_t latencyMinUs{std::numeric_limits<int64_t>::max()};
            int64_t latencyMaxUs{0};
            int64_t latencySumUs{0};
            uint64_t latencyCount{0};
            int64_t callbackMinUs{std::numeric_limits<int64_t>::max()};
            int64_t callbackMaxUs{0};
            int64_t callbackSumUs{0};
            uint64_t callbackCount{0};
        };
        std::mutex statsMutex;
        int64_t latencyMinUs{std::numeric_limits<int64_t>::max()};
        int64_t latencyMaxUs{0};
        int64_t latencySumUs{0};
        uint64_t latencyCount{0};
        int64_t callbackMinUs{std::numeric_limits<int64_t>::max()};
        int64_t callbackMaxUs{0};
        int64_t callbackSumUs{0};
        uint64_t callbackCount{0};
        int64_t spinMinUs{std::numeric_limits<int64_t>::max()};
        int64_t spinMaxUs{0};
        int64_t spinSumUs{0};
        uint64_t spinCount{0};
        uint64_t spinActiveCount{0};
        uint64_t spinIdleCount{0};
        std::unordered_map<std::string, ChannelStatsAccumulator> channelStats;

        explicit Impl(const std::string& /*provider*/)
        {
            // Per-instance id, used as the ACK subscriber_id. Random so two
            // Transport instances in one process never collide.
            std::random_device rd;
            uint64_t a = (static_cast<uint64_t>(rd()) << 32) ^ rd();
            char b[17];
            std::snprintf(b, sizeof(b), "%016llx",
                          static_cast<unsigned long long>(a));
            instanceId.assign(b);
        }

        ~Impl() { shutdown(); }

        void shutdown()
        {
            if (!running.exchange(false)) return;
            // Stop the reliability thread BEFORE taking apiMutex: the thread
            // grabs apiMutex on every tick, so joining while holding it would
            // deadlock.
            reliableThreadRun.store(false);
            if (reliableThread.joinable()) reliableThread.join();
            std::lock_guard<std::recursive_mutex> lk(apiMutex);
            if (ackReader) { rrb_reader_close(ackReader); ackReader = nullptr; }
            for (auto& [ch, pe] : publishers)
            {
                if (pe.writer) rrb_writer_destroy(pe.writer);
            }
            publishers.clear();
            for (auto& sub : subscribers)
            {
                if (sub->reader) rrb_reader_close(sub->reader);
            }
            subscribers.clear();
        }

        // ---- Reliable delivery ---------------------------------------------
        //
        // At-least-once for discrete commands, layered ON TOP of the raw SHM
        // ring without changing the data-channel wire format (so best-effort
        // subscribers keep working). Correlation key is the message's own
        // `timestamp` (first 8 bytes, big-endian) — unique per publisher.
        //
        // Publisher: publishRaw(reliable=true) sends the raw payload AND
        // records it in `pendingReliable`. A background thread re-sends any
        // entry not ACKed within its retryInterval, up to maxRetries, then
        // gives up with a loud cerr WARN. ACKs arrive on Channels::Protocol::ACK
        // and clear the matching pending entry.
        //
        // Subscriber: subscribeRaw(reliable=true) wraps the handler so that
        // every received frame is ACKed (by timestamp) and re-sends are
        // de-duplicated (handler fires once per unique command).
        struct PendingReliable
        {
            std::string channel;
            std::vector<uint8_t> payload;
            int64_t key;   // == message timestamp
            std::chrono::steady_clock::time_point lastSent;
            uint32_t attempts;
            uint32_t maxRetries;
            std::chrono::milliseconds retryInterval;
            bool warnedRetransmit;
        };
        std::vector<PendingReliable> pendingReliable;
        rrb_reader_t* ackReader = nullptr;  // dedicated reader on Protocol::ACK
        std::string instanceId;
        std::thread reliableThread;
        std::atomic<bool> reliableThreadRun{false};
        bool reliableStarted = false;
        std::atomic<uint64_t> reliableRetransmits{0};
        std::atomic<uint64_t> reliableDropped{0};

        // Lazily open the ACK reader and start the retry thread. Call under
        // apiMutex. Only the PUBLISH side needs this (it has pending entries
        // to retry and ACKs to consume); the subscribe side only needs
        // instanceId to stamp its ACKs.
        void ensureReliableStarted()
        {
            if (reliableStarted) return;
            reliableStarted = true;
            if (!ackReader)
                ackReader = rrb_reader_open(raccoon::Channels::Protocol::ACK);
            reliableThreadRun.store(true);
            reliableThread = std::thread([this] { reliableLoop(); });
        }

        void reliableLoop()
        {
            while (reliableThreadRun.load())
            {
                {
                    std::lock_guard<std::recursive_mutex> lk(apiMutex);
                    if (!running.load()) break;
                    drainAcks();
                    tickResends();
                }
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(kReliabilityTickMs));
            }
        }

        // Consume ACKs and clear matching pending entries. Call under apiMutex.
        void drainAcks()
        {
            if (!ackReader) return;
            uint8_t buf[256];
            for (;;)
            {
                size_t out = 0;
                int rc = rrb_reader_recv(ackReader, buf, sizeof(buf), &out);
                if (rc != 0) break;  // 1 = no data, -1 = error
                ack_t ack{};
                if (ack.decode(buf, static_cast<int>(out)) < 0) continue;
                const int64_t key = ack.seq_num;
                pendingReliable.erase(
                    std::remove_if(pendingReliable.begin(),
                                   pendingReliable.end(),
                                   [key](const PendingReliable& p)
                                   { return p.key == key; }),
                    pendingReliable.end());
            }
        }

        // Re-send due pending commands, drop exhausted ones. Call under apiMutex.
        void tickResends()
        {
            if (pendingReliable.empty()) return;
            const auto now = std::chrono::steady_clock::now();
            std::vector<PendingReliable> keep;
            keep.reserve(pendingReliable.size());
            for (auto& p : pendingReliable)
            {
                if (now - p.lastSent < p.retryInterval)
                {
                    keep.push_back(std::move(p));
                    continue;
                }
                if (p.attempts >= p.maxRetries)
                {
                    std::cerr << "raccoon::Transport: RELIABLE command on '"
                              << p.channel << "' (ts=" << p.key
                              << ") NOT acked after " << p.attempts
                              << " attempts — GIVING UP; the subscriber never "
                                 "received it\n";
                    reliableDropped.fetch_add(1, std::memory_order_relaxed);
                    continue;  // drop
                }
                rrb_writer_t* w = writerFor(p.channel, p.payload.size());
                if (w) rrb_writer_publish(w, p.payload.data(), p.payload.size());
                p.lastSent = now;
                ++p.attempts;
                reliableRetransmits.fetch_add(1, std::memory_order_relaxed);
                if (!p.warnedRetransmit)
                {
                    p.warnedRetransmit = true;
                    std::cerr << "raccoon::Transport: reliable command on '"
                              << p.channel << "' (ts=" << p.key
                              << ") not acked within " << p.retryInterval.count()
                              << "ms — retransmitting\n";
                }
                keep.push_back(std::move(p));
            }
            pendingReliable.swap(keep);
        }

        // Record a just-published payload for retry. Call under apiMutex.
        void recordReliable(const std::string& channel, const void* data,
                            int dataLen, const PublishOptions& options)
        {
            // The ACK channel itself must never be reliable (would recurse).
            if (channel == raccoon::Channels::Protocol::ACK) return;
            const int64_t key = readTimestampBE(data, dataLen);
            if (key == 0) return;  // no timestamp → cannot correlate an ACK
            ensureReliableStarted();
            PendingReliable p;
            p.channel = channel;
            p.payload.assign(static_cast<const uint8_t*>(data),
                             static_cast<const uint8_t*>(data) + dataLen);
            p.key = key;
            p.lastSent = std::chrono::steady_clock::now();
            p.attempts = 1;
            p.maxRetries = options.maxRetries;
            p.retryInterval = options.retryInterval;
            p.warnedRetransmit = false;
            pendingReliable.push_back(std::move(p));
        }

        // Subscriber side: publish an ACK for a received reliable frame,
        // correlated by the command's timestamp. Takes apiMutex itself
        // (called from a handler, which runs with the mutex released).
        void publishAck(int64_t key)
        {
            if (key == 0 || !running.load()) return;
            ack_t ack{};
            ack.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            ack.publisher_id.clear();  // raw wire carries no publisher id
            ack.seq_num = key;         // correlate on the command's timestamp
            ack.subscriber_id = instanceId;
            const int n = ack.encoded_size();
            if (n <= 0) return;
            std::vector<uint8_t> buf(static_cast<size_t>(n));
            const int enc = ack.encode(buf.data(), n);
            if (enc < 0) return;
            std::lock_guard<std::recursive_mutex> lk(apiMutex);
            if (!running.load()) return;
            rrb_writer_t* w = writerFor(raccoon::Channels::Protocol::ACK,
                                        static_cast<size_t>(enc));
            if (w) rrb_writer_publish(w, buf.data(), static_cast<size_t>(enc));
        }

        bool isAlive() const noexcept { return running.load(); }

        // Get-or-create a publisher writer for the channel. The first
        // publish carries a `size_hint` so we can size the ring to the
        // caller's actual payload size instead of paying the worst-case
        // for every channel.
        //
        // Sizing policy: aim for ~128 KiB total ring memory per channel,
        // with at least 4 slots of history and enough slot bytes to fit
        // up to 2× the first-seen payload. Keeps small sensor channels
        // cheap (~128 KiB) while letting camera-frame channels (256 KiB+
        // payloads) round-trip without TruncationError.
        //
        // Returns nullptr if rrb_writer_create rejected the channel name
        // (bad characters) or could not open /dev/shm — those are hard
        // configuration errors, not transients.
        // Compute (slot_count, max_payload) for a ring sized to fit
        // `size_hint` bytes with 2× headroom. Shared by writerFor (first
        // publish) and growWriter (later publish that outgrew the ring).
        static void sizeRing(size_t size_hint, size_t& slots, size_t& want)
        {
            // Round size_hint UP to a multiple of 256 bytes, double it
            // for headroom (next sensor frame may be slightly larger),
            // and clamp at a sane floor.
            want = (size_hint ? size_hint : 1) * 2;
            if (want < RRB_DEFAULT_MAX_PAYLOAD) want = RRB_DEFAULT_MAX_PAYLOAD;
            // round up to 256 bytes
            want = ((want + 255) / 256) * 256;

            // ~128 KiB ring budget; clamp slot count to [4, 64].
            slots = (128u * 1024u) / want;
            if (slots < 4) slots = 4;
            if (slots > RRB_DEFAULT_SLOT_COUNT) slots = RRB_DEFAULT_SLOT_COUNT;
        }

        rrb_writer_t* writerFor(const std::string& channel, size_t size_hint)
        {
            auto it = publishers.find(channel);
            if (it != publishers.end()) return it->second.writer;

            size_t slots = 0, want = 0;
            sizeRing(size_hint, slots, want);

            rrb_writer_t* w = rrb_writer_create(
                channel.c_str(),
                (uint32_t)slots,
                (uint32_t)want);
            if (!w)
            {
                std::cerr << "raccoon::Transport: rrb_writer_create('"
                          << channel << "') failed (bad channel name or "
                                        "/dev/shm not writable)\n";
                return nullptr;
            }
            publishers.emplace(channel, PubEntry{w, (uint32_t)want, {}});
            return w;
        }

        // Destroy and re-create the writer for `channel` with a ring big
        // enough for `min_payload` bytes. The underlying SHM file is
        // wiped + re-initialised in place by rrb_writer_create (header
        // contract: mismatched layout triggers ftruncate + re-init).
        // Active readers' mmaps stay valid but they will see a seq
        // discontinuity — acceptable for "publisher's payload size grew
        // mid-stream", which is the only caller. Returns nullptr if
        // re-create failed (channel is then removed from `publishers`).
        rrb_writer_t* growWriter(const std::string& channel, size_t min_payload)
        {
            auto it = publishers.find(channel);
            if (it == publishers.end()) return nullptr;

            size_t slots = 0, want = 0;
            sizeRing(min_payload, slots, want);
            if (want <= it->second.max_payload) return it->second.writer;

            rrb_writer_destroy(it->second.writer);
            rrb_writer_t* w = rrb_writer_create(
                channel.c_str(),
                (uint32_t)slots,
                (uint32_t)want);
            if (!w)
            {
                std::cerr << "raccoon::Transport: rrb_writer_create('"
                    << channel << "') failed during grow (was "
                    << it->second.max_payload << "B, wanted "
                    << want << "B)\n";
                publishers.erase(it);
                return nullptr;
            }
            it->second.writer = w;
            it->second.max_payload = (uint32_t)want;
            return w;
        }

        SubEntry* subscriberFor(const std::string& channel)
        {
            for (auto& s : subscribers)
            {
                if (s->channel == channel) return s.get();
            }
            rrb_reader_t* r = rrb_reader_open(channel.c_str());
            if (!r)
            {
                std::cerr << "raccoon::Transport: rrb_reader_open('"
                          << channel << "') failed (bad channel name)\n";
                return nullptr;
            }
            auto entry = std::make_unique<SubEntry>();
            entry->channel = channel;
            entry->reader = r;
            // Single per-subscriber scratch buffer big enough for any ring
            // sized by writerFor (up to ~512 KiB for the camera-frame
            // channels). 512 KB per subscriber is a one-time cost and
            // avoids per-recv resizing logic.
            entry->scratch.resize(512u * 1024u);
            SubEntry* raw = entry.get();
            subscribers.push_back(std::move(entry));
            return raw;
        }

        // Sniff message timestamp from the first 8 bytes (big-endian).
        // Every raccoon LCM message starts with an `int64_t timestamp`
        // microseconds-since-epoch field; the encode/decode is big-endian
        // per the LCM wire spec. Returns 0 if the buffer is too short.
        static int64_t readTimestampBE(const void* data, int len)
        {
            if (len < 8) return 0;
            const uint8_t* b = static_cast<const uint8_t*>(data);
            int64_t v = 0;
            for (int i = 0; i < 8; ++i)
                v = (v << 8) | b[i];
            return v;
        }
    };

    Transport::Transport() : impl_(std::make_unique<Impl>("")) {}
    Transport::~Transport() = default;
    Transport::Transport(Transport&&) noexcept = default;
    Transport& Transport::operator=(Transport&&) noexcept = default;

    Transport Transport::create(const std::string& provider)
    {
        Transport t;
        // The provider parameter is preserved for API compatibility with
        // the old LCM signature; raccoon_ring has no analogous concept
        // (every channel lives at /dev/shm/raccoon_ring_<encoded>) so we
        // just thread it into the Impl ctor for symmetry.
        t.impl_ = std::make_unique<Impl>(provider);
        return t;
    }

    bool Transport::publishRaw(const std::string& channel, const void* data,
                               int dataLen, const PublishOptions& options)
    {
        if (!impl_ || !impl_->isAlive() || !data || dataLen <= 0) return false;
        std::lock_guard<std::recursive_mutex> lk(impl_->apiMutex);
        rrb_writer_t* w = impl_->writerFor(channel, (size_t)dataLen);
        if (!w) return false;

        // Deduplication: when the caller opts in, drop value-channel
        // publishes whose payload is byte-identical to the previous one.
        // Command channels are never deduplicated; the policy (including
        // the timestamp-skipping comparison) lives in raccoon::dedup so the
        // Dart FFI bridge shares exactly the same behaviour.
        if (options.deduplicate)
        {
            auto it = impl_->publishers.find(channel);
            if (it != impl_->publishers.end() &&
                dedup::shouldDrop(channel, data, static_cast<size_t>(dataLen),
                                  it->second.dedupState))
            {
                impl_->dedupCount.fetch_add(1, std::memory_order_relaxed);
                return true;  // identical value — drop, report success
            }
        }

        int rc = rrb_writer_publish(w, data, static_cast<size_t>(dataLen));
        if (rc != 0)
        {
            // Only failure path in raccoon_ring's publish is "payload
            // too big for the ring's max_payload". writerFor sizes the
            // ring on first publish; if a LATER publish exceeds it we
            // tear down the writer and re-create it with a bigger ring
            // (rrb_writer_create wipes + re-inits the SHM file in place,
            // readers re-sync on their next recv). Surface the resize so
            // log readers can correlate any seq discontinuity with it.
            std::cerr << "raccoon::Transport: rrb_writer_publish('"
                      << channel << "') rejected " << dataLen
                      << "-byte payload — resizing ring\n";
            w = impl_->growWriter(channel, (size_t)dataLen);
            if (!w) return false;
            rc = rrb_writer_publish(w, data, static_cast<size_t>(dataLen));
            if (rc == 0)
            {
                if (options.reliable)
                    impl_->recordReliable(channel, data, dataLen, options);
                return true;
            }
            std::cerr << "raccoon::Transport: rrb_writer_publish('"
                << channel << "') still rejected " << dataLen
                << "-byte payload after resize\n";
            return false;
        }
        if (options.reliable)
            impl_->recordReliable(channel, data, dataLen, options);
        return true;
    }

    bool Transport::subscribeRaw(const std::string& channel, RawHandler handler,
                                 const SubscribeOptions& options)
    {
        if (!impl_ || !impl_->isAlive() || !handler) return false;

        // Reliable subscriber: wrap the handler so every received frame is
        // ACKed (correlated by the command's timestamp) and re-sends are
        // de-duplicated — the user handler fires once per unique command,
        // but the ACK is emitted for EVERY copy so the publisher stops
        // retransmitting even if the first delivery raced a duplicate.
        if (options.reliable)
        {
            Impl* impl = impl_.get();
            auto lastTs = std::make_shared<std::atomic<int64_t>>(0);
            RawHandler user = std::move(handler);
            handler = [impl, user, lastTs](const void* data, int len)
            {
                const int64_t ts = Impl::readTimestampBE(data, len);
                impl->publishAck(ts);
                if (ts != 0)
                {
                    if (ts <= lastTs->load(std::memory_order_relaxed))
                        return;  // duplicate re-send — acked, not re-delivered
                    lastTs->store(ts, std::memory_order_relaxed);
                }
                user(data, len);
            };
        }

        bool fresh_channel = false;
        {
            std::lock_guard<std::recursive_mutex> lk(impl_->apiMutex);
            const size_t pre = impl_->subscribers.size();
            auto* sub = impl_->subscriberFor(channel);
            if (!sub) return false;
            sub->handlers.push_back(std::move(handler));
            fresh_channel = (impl_->subscribers.size() > pre);
            // Retain-on-attach: if the caller asked for the latest cached
            // frame AND this is a fresh underlying subscriber (no prior
            // handler already drained the ring), seek the reader to the
            // last published seq so the next recv() hands out the cached
            // frame instead of walking from the oldest slot. Only fires
            // for fresh_channel — reusing an existing reader would
            // re-deliver an already-dispatched frame to other handlers.
            if (fresh_channel && options.requestRetained)
                rrb_reader_seek_to_latest(sub->reader);
        }
        // If a NEW channel was opened, the spin thread's waitv list no
        // longer reflects reality — wake it so it re-snapshots and
        // includes the new reader's wake_seq. Adding only a handler to
        // an existing channel doesn't need a wake.
        if (fresh_channel) wakeControl();
        return true;
    }

    int Transport::spinOnce(int timeoutMs)
    {
        if (!impl_ || !impl_->isAlive()) return 0;

        const auto t0 = std::chrono::steady_clock::now();
        int delivered = 0;

        // Snapshot the subscriber list under the mutex, then iterate.
        // Each receive happens under the mutex (rrb_reader_recv is not
        // thread-safe). Handler invocation releases the mutex so a slow
        // handler does not block unrelated transport ops on other threads.
        std::vector<Impl::SubEntry*> subsSnapshot;
        {
            std::lock_guard<std::recursive_mutex> lk(impl_->apiMutex);
            subsSnapshot.reserve(impl_->subscribers.size());
            for (auto& s : impl_->subscribers) subsSnapshot.push_back(s.get());
        }

        // Hold a snapshot of each frame + its handler list before
        // releasing the mutex to call handlers. This guarantees handlers
        // see a consistent view even if another thread adds/removes
        // subscribers concurrently.
        for (auto* sub : subsSnapshot)
        {
            while (true)
            {
                std::vector<uint8_t> frame;
                std::vector<Transport::RawHandler> handlersCopy;
                {
                    std::lock_guard<std::recursive_mutex> lk(impl_->apiMutex);
                    if (!impl_->isAlive()) return delivered;
                    size_t outLen = 0;
                    int rc = rrb_reader_recv(sub->reader,
                                             sub->scratch.data(),
                                             sub->scratch.size(),
                                             &outLen);
                    if (rc != 0) break;  // 1 = no data, -1 = error
                    frame.assign(sub->scratch.begin(),
                                 sub->scratch.begin() + outLen);
                    handlersCopy = sub->handlers;
                }

                // Latency: now - msg timestamp (microseconds).
                int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                int64_t msgUs = Impl::readTimestampBE(frame.data(),
                                                     (int)frame.size());
                if (msgUs > 0)
                {
                    recordLatency(sub->channel, nowUs - msgUs);
                }

                const auto cbStart = std::chrono::steady_clock::now();
                for (auto& h : handlersCopy)
                {
                    try { h(frame.data(), (int)frame.size()); }
                    catch (...) { /* swallow — handlers must not abort spin */ }
                }
                const auto cbEnd = std::chrono::steady_clock::now();
                recordCallback(sub->channel,
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        cbEnd - cbStart).count());

                ++delivered;
            }
        }

        // Idle path: pure event-driven park. The control word goes into
        // the futex_waitv list alongside every subscriber's wake_seq.
        // Wakes within microseconds on ANY publish OR on a wakeControl()
        // call from subscribe / shutdown / stop. The `timeoutMs`
        // argument is now a watchdog upper bound — a sane caller passes
        // something on the order of seconds; the thread does NOT poll
        // and does NOT wake periodically otherwise.
        //
        // timeoutMs == 0 keeps `spinOnce` non-blocking: return immediately
        // after the pass-1 drain. This preserves the historical contract
        // for callers that only want to flush pending frames once
        // (tests, manual ticks).
        if (delivered == 0 && timeoutMs > 0)
        {
            int timeoutUs = timeoutMs * 1000;
            uint32_t controlExpected =
                impl_->controlSeq.load(std::memory_order_acquire);

            struct MultiCtx {
                Transport* t;
                Impl::SubEntry** entries;
                int* delivered_out;
            };
            std::vector<Impl::SubEntry*> entries(subsSnapshot);
            std::vector<rrb_reader_t*> readers;
            readers.reserve(entries.size());
            {
                std::lock_guard<std::recursive_mutex> lk(impl_->apiMutex);
                if (!impl_->isAlive()) return delivered;
                for (auto* e : entries) readers.push_back(e->reader);
            }
            MultiCtx ctx{this, entries.data(), &delivered};
            auto cb = [](size_t idx, const void* data, size_t len,
                         void* user) -> int {
                auto* c = static_cast<MultiCtx*>(user);
                Impl::SubEntry* sub = c->entries[idx];
                int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                int64_t msgUs = Impl::readTimestampBE(data, (int)len);
                if (msgUs > 0)
                    c->t->recordLatency(sub->channel, nowUs - msgUs);
                std::vector<Transport::RawHandler> handlersCopy;
                {
                    // recordLatency takes its own lock; handler list
                    // copy needs apiMutex.
                    handlersCopy = sub->handlers;
                }
                for (auto& h : handlersCopy) {
                    try { h(data, (int)len); }
                    catch (...) {}
                }
                ++(*c->delivered_out);
                return 0;
            };
            if (readers.empty())
            {
                // Only the control word to park on. recv_wait_many's
                // contract requires n >= 1 readers, so we hand-park
                // on the control futex directly using a private
                // futex_wait — equivalent semantics, no readers needed.
                struct timespec ts = {
                    timeoutUs / 1000000,
                    (long)(timeoutUs % 1000000) * 1000L,
                };
                (void)syscall(SYS_futex, &impl_->controlSeq, FUTEX_WAIT,
                              controlExpected, &ts, nullptr, 0);
            }
            else
            {
                (void)rrb_reader_recv_wait_many_with_control(
                    readers.data(), readers.size(),
                    reinterpret_cast<uint32_t*>(&impl_->controlSeq),
                    controlExpected,
                    cb, &ctx, timeoutUs);
            }
        }

        const auto t1 = std::chrono::steady_clock::now();
        recordSpin(delivered,
            std::chrono::duration_cast<std::chrono::microseconds>(
                t1 - t0).count());
        return delivered;
    }

    void Transport::spin()
    {
        while (impl_ && impl_->isAlive())
        {
            spinOnce(10);
        }
    }

    void Transport::stop()
    {
        if (impl_) impl_->running.store(false);
        wakeControl();  // unblock any thread parked in spinOnce
    }

    void Transport::shutdown()
    {
        if (!impl_) return;
        impl_->shutdown();
        // wakeControl() works even after shutdown — the controlSeq lives
        // in Impl, which we still own, and the futex word doesn't care.
        // Without this, anyone still parked in spinOnce would wait up to
        // the watchdog timeout (1 s) for their wait to expire.
        wakeControl();
    }

    void Transport::wakeControl()
    {
        if (!impl_) return;
        // Bump the value (release-ordered against any state the waker
        // wants the spin thread to observe — running flag, subscriber
        // list, etc.) then FUTEX_WAKE so a parked spin thread exits its
        // wait immediately. The waker side never blocks: FUTEX_WAKE on
        // a word with no waiters is a ~50 ns no-op.
        std::atomic_fetch_add_explicit(&impl_->controlSeq, uint32_t{1},
                                       std::memory_order_release);
        syscall(SYS_futex, &impl_->controlSeq, FUTEX_WAKE, INT_MAX,
                nullptr, nullptr, 0);
    }

    bool Transport::is_alive() const noexcept
    {
        return impl_ && impl_->isAlive();
    }

    // ---- Stats wiring (drop-in replacement for the iceoryx2 version) ---

    void Transport::recordLatency(const std::string& channel, int64_t us)
    {
        if (!impl_ || us < 0) return;
        std::lock_guard<std::mutex> lk(impl_->statsMutex);
        impl_->latencyMinUs = std::min(impl_->latencyMinUs, us);
        impl_->latencyMaxUs = std::max(impl_->latencyMaxUs, us);
        impl_->latencySumUs += us;
        ++impl_->latencyCount;
        auto& ch = impl_->channelStats[channel];
        ch.latencyMinUs = std::min(ch.latencyMinUs, us);
        ch.latencyMaxUs = std::max(ch.latencyMaxUs, us);
        ch.latencySumUs += us;
        ++ch.latencyCount;
        // ch.deliveries is bumped in recordCallback() so payloads without
        // a parseable timestamp prefix still register a delivery.
    }

    void Transport::recordCallback(const std::string& channel, int64_t us)
    {
        if (!impl_) return;
        std::lock_guard<std::mutex> lk(impl_->statsMutex);
        impl_->callbackMinUs = std::min(impl_->callbackMinUs, us);
        impl_->callbackMaxUs = std::max(impl_->callbackMaxUs, us);
        impl_->callbackSumUs += us;
        ++impl_->callbackCount;
        auto& ch = impl_->channelStats[channel];
        ch.callbackMinUs = std::min(ch.callbackMinUs, us);
        ch.callbackMaxUs = std::max(ch.callbackMaxUs, us);
        ch.callbackSumUs += us;
        ++ch.callbackCount;
        // Count every frame we hand off to a user handler, not only the
        // ones whose payload carried a parseable timestamp prefix.
        // recordLatency() also ticks deliveries when it runs, but it short-
        // circuits on payloads < 8 bytes — without this bump, callers that
        // publish small raw frames would see zero deliveries reported.
        ++ch.deliveries;
    }

    void Transport::recordSpin(int result, int64_t us)
    {
        if (!impl_) return;
        std::lock_guard<std::mutex> lk(impl_->statsMutex);
        impl_->spinMinUs = std::min(impl_->spinMinUs, us);
        impl_->spinMaxUs = std::max(impl_->spinMaxUs, us);
        impl_->spinSumUs += us;
        ++impl_->spinCount;
        if (result > 0) ++impl_->spinActiveCount; else ++impl_->spinIdleCount;
    }

    TransportStats Transport::getAndResetStats()
    {
        TransportStats out{};
        if (!impl_) return out;
        std::lock_guard<std::mutex> lk(impl_->statsMutex);

        auto fillLat = [](TransportStats::Latency& o,
                          int64_t minUs, int64_t maxUs, int64_t sumUs,
                          uint64_t count) {
            o.count = count;
            if (count) {
                o.minUs = minUs;
                o.maxUs = maxUs;
                o.avgUs = sumUs / (int64_t)count;
                o.p99Us = maxUs;  // we don't keep samples for true p99;
                                  // surface max as a conservative upper bound
            }
        };
        auto fillCb = [](TransportStats::Callback& o,
                         int64_t minUs, int64_t maxUs, int64_t sumUs,
                         uint64_t count) {
            o.count = count; o.totalUs = sumUs;
            if (count) {
                o.minUs = minUs; o.maxUs = maxUs;
                o.avgUs = sumUs / (int64_t)count;
            }
        };

        fillLat(out.latency, impl_->latencyMinUs, impl_->latencyMaxUs,
                impl_->latencySumUs, impl_->latencyCount);
        fillCb(out.callback, impl_->callbackMinUs, impl_->callbackMaxUs,
               impl_->callbackSumUs, impl_->callbackCount);
        out.spin.count = impl_->spinCount;
        out.spin.activeCount = impl_->spinActiveCount;
        out.spin.idleCount = impl_->spinIdleCount;
        if (impl_->spinCount) {
            out.spin.minUs = impl_->spinMinUs;
            out.spin.maxUs = impl_->spinMaxUs;
            out.spin.avgUs = impl_->spinSumUs / (int64_t)impl_->spinCount;
        }
        out.publishesDeduplicated =
            impl_->dedupCount.exchange(0, std::memory_order_relaxed);
        out.reliableRetransmits =
            impl_->reliableRetransmits.exchange(0, std::memory_order_relaxed);
        out.reliableDropped =
            impl_->reliableDropped.exchange(0, std::memory_order_relaxed);

        for (auto& [ch, acc] : impl_->channelStats) {
            TransportStats::Channel c{};
            c.name = ch;
            c.deliveries = acc.deliveries;
            fillLat(c.latency, acc.latencyMinUs, acc.latencyMaxUs,
                    acc.latencySumUs, acc.latencyCount);
            fillCb(c.callback, acc.callbackMinUs, acc.callbackMaxUs,
                   acc.callbackSumUs, acc.callbackCount);
            out.channels.push_back(std::move(c));
        }

        // Reset accumulators (same semantics as the iceoryx2 version)
        impl_->latencyMinUs = std::numeric_limits<int64_t>::max();
        impl_->latencyMaxUs = 0;
        impl_->latencySumUs = 0;
        impl_->latencyCount = 0;
        impl_->callbackMinUs = std::numeric_limits<int64_t>::max();
        impl_->callbackMaxUs = 0;
        impl_->callbackSumUs = 0;
        impl_->callbackCount = 0;
        impl_->spinMinUs = std::numeric_limits<int64_t>::max();
        impl_->spinMaxUs = 0;
        impl_->spinSumUs = 0;
        impl_->spinCount = 0;
        impl_->spinActiveCount = 0;
        impl_->spinIdleCount = 0;
        impl_->channelStats.clear();
        return out;
    }
}
