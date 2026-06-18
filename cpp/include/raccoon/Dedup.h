#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "raccoon/Channels.h"

// Shared value-channel deduplication used by every C++ publish path
// (raccoon::Transport for the stm32-data-reader / raccoon-lib side, and
// the Dart FFI bridge for botui). Keeping the policy here means the
// command-vs-value distinction and the timestamp-skipping comparison live
// in exactly one place — Dart and other consumers call into this rather
// than reimplementing it.
namespace raccoon::dedup
{
    // Per-channel record of the last value bytes published with
    // deduplicate=true, stored WITHOUT the leading 8-byte timestamp.
    struct LastPayload
    {
        std::vector<uint8_t> bytes;
        bool valid = false;
    };

    // Decide whether a deduplicate=true publish on `channel` with payload
    // [data, len) should be DROPPED as a byte-identical repeat, updating
    // `state` to remember this value when it is not a duplicate.
    //
    // Policy:
    //   * Command channels (Channels::isCommandChannel) are NEVER
    //     deduplicated — re-issuing the same command is meaningful and must
    //     always reach the subscriber. Returns false and leaves `state`
    //     untouched.
    //   * The comparison ignores the leading 8-byte timestamp that every
    //     raccoon value type stamps fresh on each publish (payloads of 8
    //     bytes or fewer are compared whole).
    //   * Returns true iff the value bytes match the previous publish.
    inline bool shouldDrop(const std::string& channel, const void* data,
                           size_t len, LastPayload& state)
    {
        if (Channels::isCommandChannel(channel)) return false;

        const auto* bytes = static_cast<const uint8_t*>(data);
        const size_t off = (len > 8) ? 8u : 0u;
        const size_t cmpLen = len - off;

        if (state.valid && state.bytes.size() == cmpLen &&
            std::memcmp(state.bytes.data(), bytes + off, cmpLen) == 0)
        {
            return true;
        }
        state.bytes.assign(bytes + off, bytes + off + cmpLen);
        state.valid = true;
        return false;
    }
}  // namespace raccoon::dedup
