#pragma once

#include <lcm/lcm.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace raccoon::detail
{
    class RetainStore
    {
    public:
        void cache(const std::string& channel, const void* data, int dataLen);
        bool get(const std::string& channel, std::vector<uint8_t>& out) const;
        void startListening(lcm_t* lcm);

    private:
        static void onRetainRequest(const lcm_recv_buf_t* rbuf,
                                    const char* channel, void* userdata);

        mutable std::mutex mutex_;
        std::unordered_map<std::string, std::vector<uint8_t>> cache_;
        lcm_t* lcm_{nullptr};
    };
}
