#include "raccoon/Channels.h"
#include "raccoon/Options.h"
#include "raccoon/Transport.h"
#include "raccoon/detail/ReliablePublisher.h"
#include "raccoon/detail/ReliableSubscriber.h"
#include "raccoon/detail/RetainStore.h"

#include <lcm/lcm.h>
#include <raccoon/ack_t.hpp>
#include <raccoon/envelope_t.hpp>
#include <raccoon/retain_request_t.hpp>
#include <raccoon/scalar_i32_t.hpp>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
    struct TestFailure : std::runtime_error
    {
        using std::runtime_error::runtime_error;
    };

    void require(bool condition, const std::string& message)
    {
        if (!condition)
        {
            throw TestFailure(message);
        }
    }

    struct LcmHandle
    {
        explicit LcmHandle(const std::string& provider)
            : lcm(lcm_create(provider.c_str()))
        {
            require(lcm != nullptr, "failed to create LCM instance");
        }

        ~LcmHandle()
        {
            if (lcm != nullptr)
            {
                lcm_destroy(lcm);
            }
        }

        LcmHandle(const LcmHandle&) = delete;
        LcmHandle& operator=(const LcmHandle&) = delete;

        lcm_t* get() const { return lcm; }

    private:
        lcm_t* lcm;
    };

    struct SpyMessage
    {
        std::string channel;
        std::vector<uint8_t> data;
    };

    struct SpySubscription
    {
        std::vector<SpyMessage> messages;
    };

    void spyCallback(const lcm_recv_buf_t* rbuf, const char* channel, void* userdata)
    {
        auto* spy = static_cast<SpySubscription*>(userdata);
        auto* bytes = static_cast<const uint8_t*>(rbuf->data);
        spy->messages.push_back(
            SpyMessage{channel, std::vector<uint8_t>(bytes, bytes + rbuf->data_size)});
    }

    void drain(lcm_t* lcm, int maxIterations = 64)
    {
        for (int i = 0; i < maxIterations; ++i)
        {
            if (lcm_handle_timeout(lcm, 0) <= 0)
            {
                return;
            }
        }

        throw TestFailure("LCM queue did not drain within iteration budget");
    }

    void spinTransport(raccoon::Transport& transport, int maxIterations = 64)
    {
        for (int i = 0; i < maxIterations; ++i)
        {
            if (transport.spinOnce(0) <= 0)
            {
                return;
            }
        }

        throw TestFailure("transport queue did not drain within iteration budget");
    }

    std::vector<uint8_t> encodeEnvelope(const std::string& publisherId, int64_t seqNum,
                                        const std::string& channel,
                                        const std::vector<uint8_t>& payload)
    {
        raccoon::envelope_t envelope;
        envelope.timestamp = 123456789;
        envelope.publisher_id = publisherId;
        envelope.seq_num = seqNum;
        envelope.channel = channel;
        envelope.payload_size = static_cast<int32_t>(payload.size());
        envelope.payload = payload;

        std::vector<uint8_t> encoded(envelope.getEncodedSize());
        const int len = envelope.encode(encoded.data(), 0,
                                        static_cast<int>(encoded.size()));
        require(len > 0, "failed to encode envelope");
        encoded.resize(static_cast<size_t>(len));
        return encoded;
    }

    std::vector<uint8_t> encodeAck(const std::string& publisherId, int64_t seqNum,
                                   const std::string& subscriberId)
    {
        raccoon::ack_t ack;
        ack.timestamp = 987654321;
        ack.publisher_id = publisherId;
        ack.seq_num = seqNum;
        ack.subscriber_id = subscriberId;

        std::vector<uint8_t> encoded(ack.getEncodedSize());
        const int len = ack.encode(encoded.data(), 0, static_cast<int>(encoded.size()));
        require(len > 0, "failed to encode ack");
        encoded.resize(static_cast<size_t>(len));
        return encoded;
    }

    std::vector<uint8_t> encodeRetainRequestManual(const std::string& channel)
    {
        const auto chanLen = static_cast<int32_t>(channel.size());
        std::vector<uint8_t> buf(static_cast<size_t>(8 + 8 + 4 + chanLen + 4), 0);
        int off = 16;
        buf[off++] = static_cast<uint8_t>((chanLen >> 24) & 0xFF);
        buf[off++] = static_cast<uint8_t>((chanLen >> 16) & 0xFF);
        buf[off++] = static_cast<uint8_t>((chanLen >> 8) & 0xFF);
        buf[off++] = static_cast<uint8_t>(chanLen & 0xFF);
        std::memcpy(buf.data() + off, channel.data(), static_cast<size_t>(chanLen));
        return buf;
    }

    void testTransportDeduplicatesExactPayloads()
    {
        std::vector<std::vector<uint8_t>> received;
        auto single = raccoon::Transport::create("memq://");
        require(single.subscribeRaw("unit/dedup",
            [&](const void* data, int len)
            {
                const auto* bytes = static_cast<const uint8_t*>(data);
                received.emplace_back(bytes, bytes + len);
            }), "failed to subscribe capture handler");

        const std::vector<uint8_t> first{1, 2, 3, 4};
        const std::vector<uint8_t> duplicate{1, 2, 3, 4};
        const std::vector<uint8_t> changed{1, 2, 3, 5};

        raccoon::PublishOptions options;
        options.deduplicate = true;

        require(single.publishRaw("unit/dedup", first.data(), static_cast<int>(first.size()), options),
                "first publish failed");
        require(single.publishRaw("unit/dedup", duplicate.data(), static_cast<int>(duplicate.size()), options),
                "duplicate publish should be treated as success");
        require(single.publishRaw("unit/dedup", changed.data(), static_cast<int>(changed.size()), options),
                "changed publish failed");

        spinTransport(single);

        require(received.size() == 2, "deduplication should suppress exactly one publish");
        require(received[0] == first, "first payload should be delivered unchanged");
        require(received[1] == changed, "changed payload should not be deduplicated");

        auto stats = single.getAndResetStats();
        require(stats.publishesDeduplicated == 1, "dedup counter should record one suppressed publish");

        auto resetStats = single.getAndResetStats();
        require(resetStats.publishesDeduplicated == 0, "dedup counter should reset after read");
    }

    void testTransportRequestRetainedReplaysCachedPayload()
    {
        auto transport = raccoon::Transport::create("memq://");

        const std::vector<uint8_t> cached{9, 8, 7, 6};
        require(transport.publishRaw("unit/retain", cached.data(), static_cast<int>(cached.size()),
                raccoon::PublishOptions{.retained = true}),
                "retained publish failed");

        std::vector<std::vector<uint8_t>> retained;
        require(transport.subscribeRaw("unit/retain",
            [&](const void* data, int len)
            {
                const auto* bytes = static_cast<const uint8_t*>(data);
                retained.emplace_back(bytes, bytes + len);
            },
            raccoon::SubscribeOptions{.requestRetained = true}),
            "retained subscribe failed");

        spinTransport(transport);

        require(retained.size() == 1, "requestRetained should replay exactly one cached payload");
        require(retained[0] == cached, "replayed retained payload should match cached bytes exactly");
    }

    void testTransportRequestRetainedDoesNotInventMessages()
    {
        auto transport = raccoon::Transport::create("memq://");

        bool called = false;
        require(transport.subscribeRaw("unit/missing",
            [&](const void*, int)
            {
                called = true;
            },
            raccoon::SubscribeOptions{.requestRetained = true}),
            "subscribe for missing retained channel failed");

        spinTransport(transport);
        require(!called, "requestRetained should not emit data for an uncached channel");
    }

    void testRetainStoreAcceptsGeneratedCppRequests()
    {
        LcmHandle lcm("memq://");
        raccoon::detail::RetainStore store;
        store.startListening(lcm.get());

        const std::vector<uint8_t> payload{4, 2, 4, 2};
        store.cache("unit/cpp_request", payload.data(), static_cast<int>(payload.size()));

        SpySubscription spy;
        lcm_subscribe(lcm.get(), "unit/cpp_request", spyCallback, &spy);

        raccoon::retain_request_t request;
        request.timestamp = 111;
        request.channel = "unit/cpp_request";
        request.subscriber_id = "subscriber";

        std::vector<uint8_t> encoded(request.getEncodedSize());
        const int encodedLen = request.encode(encoded.data(), 0,
                                              static_cast<int>(encoded.size()));
        require(encodedLen > 0, "failed to encode generated retain_request_t");

        require(lcm_publish(lcm.get(), raccoon::Channels::Protocol::RETAIN_REQUEST,
                encoded.data(), static_cast<unsigned int>(encodedLen)) == 0,
                "publishing generated retain request failed");

        drain(lcm.get());

        require(spy.messages.size() == 1, "generated C++ retain request should replay cached payload");
        require(spy.messages[0].data == payload, "retain store should replay exact cached bytes");
    }

    void testRetainStoreAcceptsManualInteropRequests()
    {
        LcmHandle lcm("memq://");
        raccoon::detail::RetainStore store;
        store.startListening(lcm.get());

        const std::vector<uint8_t> payload{8, 6, 7, 5, 3, 0, 9};
        store.cache("unit/manual_request", payload.data(), static_cast<int>(payload.size()));

        SpySubscription spy;
        lcm_subscribe(lcm.get(), "unit/manual_request", spyCallback, &spy);

        const auto encoded = encodeRetainRequestManual("unit/manual_request");
        require(lcm_publish(lcm.get(), raccoon::Channels::Protocol::RETAIN_REQUEST,
                encoded.data(), static_cast<unsigned int>(encoded.size())) == 0,
                "publishing manual interop retain request failed");

        drain(lcm.get());

        require(spy.messages.size() == 1, "manual interop retain request should replay cached payload");
        require(spy.messages[0].data == payload, "manual interop replay should preserve cached bytes exactly");
    }

    void testReliableSubscriberDeduplicatesButAcksEveryDelivery()
    {
        LcmHandle lcm("memq://");
        raccoon::detail::ReliableSubscriber subscriber("subscriber-1");

        int deliveries = 0;
        std::vector<uint8_t> deliveredPayload;
        subscriber.subscribe(lcm.get(), "unit/reliable",
            [&](const void* data, int len)
            {
                ++deliveries;
                const auto* bytes = static_cast<const uint8_t*>(data);
                deliveredPayload.assign(bytes, bytes + len);
            });

        SpySubscription ackSpy;
        lcm_subscribe(lcm.get(), raccoon::Channels::Protocol::ACK, spyCallback, &ackSpy);

        const std::vector<uint8_t> payload{7, 7, 1};
        const auto envelope = encodeEnvelope("publisher-1", 42, "unit/reliable", payload);
        const auto reliableChannel = raccoon::Channels::Protocol::reliableChannel("unit/reliable");

        require(lcm_publish(lcm.get(), reliableChannel.c_str(),
                envelope.data(), static_cast<unsigned int>(envelope.size())) == 0,
                "first reliable publish failed");
        require(lcm_publish(lcm.get(), reliableChannel.c_str(),
                envelope.data(), static_cast<unsigned int>(envelope.size())) == 0,
                "duplicate reliable publish failed");

        drain(lcm.get());

        require(deliveries == 1, "duplicate reliable envelopes should be delivered exactly once");
        require(deliveredPayload == payload, "reliable subscriber should expose the exact inner payload");
        require(ackSpy.messages.size() == 2, "subscriber should ACK every envelope delivery, including duplicates");

        for (const auto& msg : ackSpy.messages)
        {
            raccoon::ack_t ack;
            require(ack.decode(msg.data.data(), 0, static_cast<int>(msg.data.size())) >= 0,
                    "failed to decode ACK emitted by subscriber");
            require(ack.publisher_id == "publisher-1", "ACK publisher_id should match envelope publisher");
            require(ack.seq_num == 42, "ACK seq_num should match envelope seq_num");
            require(ack.subscriber_id == "subscriber-1", "ACK subscriber_id should match subscriber instance");
        }
    }

    void testReliablePublisherStopsRetryingAfterMatchingAck()
    {
        LcmHandle lcm("memq://");
        raccoon::detail::ReliablePublisher publisher("publisher-2");
        publisher.startListening(lcm.get());

        SpySubscription reliableSpy;
        lcm_subscribe(lcm.get(),
                      raccoon::Channels::Protocol::reliableChannel("unit/ack_stop").c_str(),
                      spyCallback, &reliableSpy);

        raccoon::PublishOptions options;
        options.reliable = true;
        options.retryInterval = std::chrono::milliseconds(0);
        options.maxRetries = 5;

        const std::vector<uint8_t> payload{1, 9, 9};
        require(publisher.publish(lcm.get(), "unit/ack_stop",
                payload.data(), static_cast<int>(payload.size()), options),
                "reliable publish failed");
        drain(lcm.get());

        require(reliableSpy.messages.size() == 1, "initial reliable publish should emit exactly one envelope");

        const auto ack = encodeAck("publisher-2", 0, "subscriber-2");
        require(lcm_publish(lcm.get(), raccoon::Channels::Protocol::ACK,
                ack.data(), static_cast<unsigned int>(ack.size())) == 0,
                "publishing matching ACK failed");
        drain(lcm.get());

        publisher.tick(lcm.get());
        drain(lcm.get());

        require(reliableSpy.messages.size() == 1,
                "matching ACK should remove the pending message before any retry occurs");
    }

    void testReliablePublisherRetriesExactlyUntilMaxRetries()
    {
        LcmHandle lcm("memq://");
        raccoon::detail::ReliablePublisher publisher("publisher-3");

        SpySubscription reliableSpy;
        lcm_subscribe(lcm.get(),
                      raccoon::Channels::Protocol::reliableChannel("unit/retry_budget").c_str(),
                      spyCallback, &reliableSpy);

        raccoon::PublishOptions options;
        options.reliable = true;
        options.retryInterval = std::chrono::milliseconds(0);
        options.maxRetries = 3;

        const std::vector<uint8_t> payload{5, 4, 3, 2, 1};
        require(publisher.publish(lcm.get(), "unit/retry_budget",
                payload.data(), static_cast<int>(payload.size()), options),
                "reliable publish failed");
        drain(lcm.get());

        publisher.tick(lcm.get());
        drain(lcm.get());
        publisher.tick(lcm.get());
        drain(lcm.get());
        publisher.tick(lcm.get());
        drain(lcm.get());

        require(reliableSpy.messages.size() == 3,
                "publisher should emit initial send plus retries until maxRetries is exhausted");

        for (const auto& msg : reliableSpy.messages)
        {
            raccoon::envelope_t envelope;
            require(envelope.decode(msg.data.data(), 0, static_cast<int>(msg.data.size())) >= 0,
                    "failed to decode retried envelope");
            require(envelope.publisher_id == "publisher-3", "envelope publisher_id should remain stable");
            require(envelope.seq_num == 0, "all retries must keep the original sequence number");
            require(envelope.channel == "unit/retry_budget", "inner channel should remain stable across retries");
            require(envelope.payload == payload, "envelope payload should remain byte-identical across retries");
        }
    }

    void testTypedSubscriptionRecordsAndResetsLatencyStats()
    {
        auto transport = raccoon::Transport::create("memq://");

        int callbacks = 0;
        require(transport.subscribe<raccoon::scalar_i32_t>("unit/typed",
            [&](const raccoon::scalar_i32_t& msg)
            {
                ++callbacks;
                require(msg.value == 123, "typed subscription should decode the published message");
            }),
            "typed subscribe failed");

        raccoon::scalar_i32_t msg;
        msg.timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch() - std::chrono::milliseconds(5)).count();
        msg.value = 123;

        require(transport.publish("unit/typed", msg), "typed publish failed");
        spinTransport(transport);

        require(callbacks == 1, "typed subscription should fire exactly once");

        auto stats = transport.getAndResetStats();
        require(stats.latency.count == 1, "typed delivery should record one latency sample");
        require(stats.latency.minUs > 0, "latency minimum should be positive for a past timestamp");
        require(stats.latency.maxUs >= stats.latency.minUs, "latency max should be at least latency min");
        require(stats.latency.avgUs >= stats.latency.minUs, "latency avg should be at least latency min");

        auto reset = transport.getAndResetStats();
        require(reset.latency.count == 0, "latency stats should reset after retrieval");
        require(reset.latency.minUs == 0, "reset latency min should be zeroed");
        require(reset.latency.maxUs == 0, "reset latency max should be zeroed");
        require(reset.latency.avgUs == 0, "reset latency avg should be zeroed");
    }
}

int main()
{
    const std::vector<std::pair<std::string, std::function<void()>>> tests = {
        {"transport deduplicates exact payloads", testTransportDeduplicatesExactPayloads},
        {"transport requestRetained replays cached payload", testTransportRequestRetainedReplaysCachedPayload},
        {"transport requestRetained does not invent messages", testTransportRequestRetainedDoesNotInventMessages},
        {"retain store accepts generated C++ requests", testRetainStoreAcceptsGeneratedCppRequests},
        {"retain store accepts manual interop requests", testRetainStoreAcceptsManualInteropRequests},
        {"reliable subscriber deduplicates but ACKs every delivery", testReliableSubscriberDeduplicatesButAcksEveryDelivery},
        {"reliable publisher stops retrying after matching ACK", testReliablePublisherStopsRetryingAfterMatchingAck},
        {"reliable publisher retries exactly until maxRetries", testReliablePublisherRetriesExactlyUntilMaxRetries},
        {"typed subscription records and resets latency stats", testTypedSubscriptionRecordsAndResetsLatencyStats},
    };

    for (const auto& [name, test] : tests)
    {
        try
        {
            test();
            std::cout << "[PASS] " << name << '\n';
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[FAIL] " << name << ": " << ex.what() << '\n';
            return 1;
        }
    }

    return 0;
}
