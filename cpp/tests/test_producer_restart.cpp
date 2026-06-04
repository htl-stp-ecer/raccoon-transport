#include "raccoon/Transport.h"
#include "raccoon/raccoon_ring.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

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

    class ChannelCleanup
    {
    public:
        explicit ChannelCleanup(std::string channel)
            : channel_(std::move(channel))
        {
            require(rrb_channel_to_path(channel_.c_str(), path_, sizeof(path_)) == 0,
                    "failed to derive shm path for " + channel_);
            cleanup();
        }

        ~ChannelCleanup()
        {
            cleanup();
        }

        ChannelCleanup(const ChannelCleanup&) = delete;
        ChannelCleanup& operator=(const ChannelCleanup&) = delete;

        const char* path() const
        {
            return path_;
        }

    private:
        void cleanup() const
        {
            (void)unlink(path_);
        }

        std::string channel_;
        char path_[512]{};
    };

    class ReaderPump
    {
    public:
        explicit ReaderPump(const std::string& channel)
        {
            transport_ = raccoon::Transport::create();
            require(transport_.subscribeRaw(channel,
                [this](const void* data, int len)
                {
                    const auto* bytes = static_cast<const char*>(data);
                    std::lock_guard<std::mutex> lock(mutex_);
                    messages_.emplace_back(bytes, static_cast<size_t>(len));
                }),
                "failed to subscribe reader for " + channel);
            worker_ = std::thread([this]() { run(); });
        }

        ~ReaderPump()
        {
            stop_ = true;
            if (worker_.joinable())
            {
                worker_.join();
            }
            transport_.shutdown();
        }

        std::vector<std::string> snapshot() const
        {
            std::lock_guard<std::mutex> lock(mutex_);
            return messages_;
        }

        bool waitForSuffix(const std::vector<std::string>& expected,
                           std::chrono::milliseconds timeout) const
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (messages_.size() >= expected.size())
                    {
                        const size_t start = messages_.size() - expected.size();
                        bool match = true;
                        for (size_t i = 0; i < expected.size(); ++i)
                        {
                            if (messages_[start + i] != expected[i])
                            {
                                match = false;
                                break;
                            }
                        }
                        if (match)
                        {
                            return true;
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            return false;
        }

    private:
        void run()
        {
            while (!stop_)
            {
                if (transport_.spinOnce(0) <= 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
        }

        raccoon::Transport transport_;
        mutable std::mutex mutex_;
        std::vector<std::string> messages_;
        std::thread worker_;
        bool stop_ = false;
    };

    void publishMessages(rrb_writer_t* writer, const std::string& prefix, int count)
    {
        require(writer != nullptr, "writer is null");
        for (int i = 0; i < count; ++i)
        {
            const std::string payload = prefix + std::to_string(i);
            require(rrb_writer_publish(writer, payload.data(), payload.size()) == 0,
                    "publish failed for payload " + payload);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    std::vector<std::string> makeExpected(const std::string& prefix, int count)
    {
        std::vector<std::string> out;
        out.reserve(static_cast<size_t>(count));
        for (int i = 0; i < count; ++i)
        {
            out.push_back(prefix + std::to_string(i));
        }
        return out;
    }

    void testProducerRestart()
    {
        const std::string channel = "raccoon/test/restart";
        ChannelCleanup cleanup(channel);

        rrb_writer_t* writer = rrb_writer_create(channel.c_str(), 64, 256);
        require(writer != nullptr, "failed to create writer");
        ReaderPump reader(channel);

        publishMessages(writer, "hello-", 100);
        rrb_writer_destroy(writer);

        writer = rrb_writer_create(channel.c_str(), 64, 256);
        require(writer != nullptr, "failed to recreate writer");
        publishMessages(writer, "world-", 10);

        const auto expected = makeExpected("world-", 10);
        const bool ok = reader.waitForSuffix(expected, std::chrono::seconds(2));
        if (!ok)
        {
            const auto got = reader.snapshot();
            std::fprintf(stderr, "test_producer_restart received=%zu\n", got.size());
            const size_t start = got.size() > 12 ? got.size() - 12 : 0;
            for (size_t i = start; i < got.size(); ++i)
            {
                std::fprintf(stderr, "  got[%zu]=%s\n", i, got[i].c_str());
            }
        }
        require(ok, "reader did not receive world-0..world-9 after producer restart");

        rrb_writer_destroy(writer);
    }

    void testProducerRestartAfterLapped()
    {
        const std::string channel = "raccoon/test/restart_lapped";
        ChannelCleanup cleanup(channel);

        rrb_writer_t* writer = rrb_writer_create(channel.c_str(), 64, 256);
        require(writer != nullptr, "failed to create writer");
        ReaderPump reader(channel);

        publishMessages(writer, "lap-", 256);
        rrb_writer_destroy(writer);

        writer = rrb_writer_create(channel.c_str(), 64, 256);
        require(writer != nullptr, "failed to recreate writer");
        publishMessages(writer, "world-", 10);

        const auto expected = makeExpected("world-", 10);
        require(reader.waitForSuffix(expected, std::chrono::seconds(2)),
                "reader did not receive world-0..world-9 after lapped producer restart");

        rrb_writer_destroy(writer);
    }

    void testSubscriberBeforeProducer()
    {
        const std::string channel = "raccoon/test/subscriber_before_producer";
        ChannelCleanup cleanup(channel);

        ReaderPump reader(channel);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        rrb_writer_t* writer = rrb_writer_create(channel.c_str(), 64, 256);
        require(writer != nullptr, "failed to create writer");
        publishMessages(writer, "first-", 10);

        const auto expected = makeExpected("first-", 10);
        require(reader.waitForSuffix(expected, std::chrono::seconds(2)),
                "reader did not receive first-0..first-9 when subscriber started first");

        rrb_writer_destroy(writer);
    }

    void runTest(const char* name, const std::function<void()>& fn)
    {
        try
        {
            fn();
            std::fprintf(stderr, "[PASS] %s\n", name);
        }
        catch (const std::exception& ex)
        {
            std::fprintf(stderr, "[FAIL] %s: %s\n", name, ex.what());
            throw;
        }
    }
}

int main(int argc, char** argv)
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"test_producer_restart", testProducerRestart},
        {"test_producer_restart_after_lapped", testProducerRestartAfterLapped},
        {"test_subscriber_before_producer", testSubscriberBeforeProducer},
    };

    if (argc > 1)
    {
        const std::string wanted = argv[1];
        for (const auto& [name, fn] : tests)
        {
            if (wanted == name)
            {
                runTest(name, fn);
                return 0;
            }
        }
        std::fprintf(stderr, "unknown test %s\n", argv[1]);
        return 2;
    }

    for (const auto& [name, fn] : tests)
    {
        runTest(name, fn);
    }
    return 0;
}
