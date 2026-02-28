#include "raccoon/Transport.h"
#include <lcm/lcm-cpp.hpp>
#include <lcm/lcm.h>
#include <iostream>
#include <atomic>
#include <vector>

namespace raccoon
{
    // Raw subscribe callback - forwarded from C API to stored std::function
    struct RawSubscription
    {
        Transport::RawHandler handler;
    };

    static void rawSubscribeCallback(const lcm_recv_buf_t* rbuf, const char*,
                                     void* userdata)
    {
        auto* sub = static_cast<RawSubscription*>(userdata);
        sub->handler(rbuf->data, static_cast<int>(rbuf->data_size));
    }

    class Transport::Impl
    {
    public:
        lcm::LCM lcm;
        std::atomic<bool> running{false};
        std::vector<std::unique_ptr<RawSubscription>> subscriptions;

        bool initialize(const std::string& provider)
        {
            if (provider.empty())
                lcm = lcm::LCM();
            else
                lcm = lcm::LCM(provider);

            return lcm.good();
        }
    };

    Transport::Transport() = default;
    Transport::~Transport() = default;
    Transport::Transport(Transport&&) noexcept = default;
    Transport& Transport::operator=(Transport&&) noexcept = default;

    Transport Transport::create(const std::string& provider)
    {
        Transport t;
        t.impl_ = std::make_unique<Impl>();
        if (!t.impl_->initialize(provider))
        {
            std::cerr << "raccoon::Transport: Failed to initialize LCM" << std::endl;
        }
        return t;
    }

    bool Transport::publishRaw(const std::string& channel, const void* data, int dataLen,
                               const PublishOptions& options)
    {
        if (!impl_ || !impl_->lcm.good()) return false;

        if (options.reliable || options.retained)
        {
            std::cerr << "raccoon::Transport: reliable/retained not yet implemented, "
                      << "falling back to plain publish on: " << channel << std::endl;
        }

        return impl_->lcm.publish(channel, data, static_cast<unsigned int>(dataLen)) == 0;
    }

    bool Transport::subscribeRaw(const std::string& channel, RawHandler handler,
                                 const SubscribeOptions& options)
    {
        if (!impl_ || !impl_->lcm.good()) return false;

        if (options.reliable || options.requestRetained)
        {
            std::cerr << "raccoon::Transport: reliable/retained not yet implemented, "
                      << "falling back to plain subscribe on: " << channel << std::endl;
        }

        auto sub = std::make_unique<RawSubscription>();
        sub->handler = std::move(handler);
        auto* subPtr = sub.get();
        impl_->subscriptions.push_back(std::move(sub));

        lcm_subscribe(impl_->lcm.getUnderlyingLCM(), channel.c_str(),
                      rawSubscribeCallback, subPtr);

        return true;
    }

    int Transport::spinOnce(int timeoutMs)
    {
        if (!impl_ || !impl_->lcm.good()) return -1;
        return impl_->lcm.handleTimeout(timeoutMs);
    }

    void Transport::spin()
    {
        if (!impl_ || !impl_->lcm.good()) return;
        impl_->running = true;
        while (impl_->running)
        {
            impl_->lcm.handleTimeout(100);
        }
    }

    void Transport::stop()
    {
        if (impl_) impl_->running = false;
    }
}
