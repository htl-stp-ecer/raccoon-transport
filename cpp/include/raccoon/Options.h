#pragma once

namespace raccoon
{
    /**
     * Options of a publish. Whether a channel is reliable or retained is a property of
     * the channel table (`channels.yml`), not of a call: a publish on a reliable
     * channel is re-sent until the subscriber acknowledges it, and a subscriber of
     * such a channel acknowledges every frame and receives each command once.
     */
    struct PublishOptions
    {
        /// Drop a publish whose payload (without the 8-byte timestamp) is
        /// byte-identical to the previous one on this channel. Honoured only for
        /// value channels: command channels (a name with a `cmd` segment) are never
        /// deduplicated, so a repeated command always reaches the subscriber.
        bool deduplicate = false;
    };
}
