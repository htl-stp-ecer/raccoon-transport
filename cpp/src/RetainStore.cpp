#include "raccoon/detail/RetainStore.h"
#include "raccoon/Channels.h"
#include <lcm/lcm.h>
#include <cstring>

// Manual retain_request_t decoding to match Python/Dart protocol.
// Python/Dart encode strings as (int32 len + raw bytes) without the null
// terminator that LCM-generated C++ code expects, and send fingerprint=0.
// We skip the fingerprint and parse strings without null terminators so
// all three languages interoperate correctly.

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

    static int32_t readInt32BE(const uint8_t* p)
    {
        return static_cast<int32_t>(
            (static_cast<uint32_t>(p[0]) << 24) |
            (static_cast<uint32_t>(p[1]) << 16) |
            (static_cast<uint32_t>(p[2]) <<  8) |
            (static_cast<uint32_t>(p[3])));
    }

    void RetainStore::onRetainRequest(const lcm_recv_buf_t* rbuf,
                                      const char*, void* userdata)
    {
        auto* self = static_cast<RetainStore*>(userdata);
        auto* data = static_cast<const uint8_t*>(rbuf->data);
        auto size = static_cast<int>(rbuf->data_size);

        // Layout: int64 fingerprint (8) + int64 timestamp (8) +
        //         int32 channel_len + channel_bytes +
        //         int32 subscriber_id_len + subscriber_id_bytes
        // Minimum size: 8 + 8 + 4 = 20
        if (size < 20) return;

        int offset = 8;  // skip fingerprint
        offset += 8;     // skip timestamp

        int32_t chanLen = readInt32BE(data + offset);
        offset += 4;
        if (offset + chanLen > size) return;

        std::string requestedChannel(reinterpret_cast<const char*>(data + offset),
                                     chanLen);
        // Strip trailing null if present (handles both LCM-generated and manual encoding)
        if (!requestedChannel.empty() && requestedChannel.back() == '\0')
            requestedChannel.pop_back();

        std::vector<uint8_t> cached;
        if (self->get(requestedChannel, cached))
        {
            lcm_publish(self->lcm_, requestedChannel.c_str(), cached.data(),
                        static_cast<unsigned int>(cached.size()));
        }
    }
}
