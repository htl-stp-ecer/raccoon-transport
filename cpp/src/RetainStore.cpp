#include "raccoon/detail/RetainStore.h"
#include "raccoon/Channels.h"
#include <lcm/lcm.h>
#include <raccoon/retain_request_t.hpp>
#include <cstring>

namespace raccoon::detail
{
    void RetainStore::cache(const std::string& channel, const void* data, int dataLen)
    {
        std::lock_guard lock(mutex_);
        auto& entry = cache_[channel];
        entry.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + dataLen);
    }

    bool RetainStore::get(const std::string& channel, std::vector<uint8_t>& out) const
    {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(channel);
        if (it == cache_.end()) return false;
        out = it->second;
        return true;
    }

    void RetainStore::startListening(lcm_t* lcm)
    {
        lcm_ = lcm;
        lcm_subscribe(lcm, Channels::Protocol::RETAIN_REQUEST, onRetainRequest, this);
    }

    void RetainStore::onRetainRequest(const lcm_recv_buf_t* rbuf,
                                      const char*, void* userdata)
    {
        auto* self = static_cast<RetainStore*>(userdata);

        raccoon::retain_request_t msg{};
        if (msg.decode(rbuf->data, 0, static_cast<int>(rbuf->data_size)) < 0)
        {
            return;
        }

        std::vector<uint8_t> cached;
        if (self->get(msg.channel, cached))
        {
            lcm_publish(self->lcm_, msg.channel.c_str(), cached.data(),
                        static_cast<unsigned int>(cached.size()));
        }
    }
}
