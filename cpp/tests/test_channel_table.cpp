// The generated channel table: lookups, the slot size of fixed types and the flags.

#include "raccoon/ChannelTable.h"

#include <cstdio>
#include <string>

using namespace raccoon::channels;

namespace
{
    int g_failures = 0;
    void check(bool condition, const std::string& what)
    {
        if (!condition)
        {
            ++g_failures;
            std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        }
    }
}

int main()
{
    // Every entry can be found by its name and names are unique.
    for (std::size_t i = 0; i < CHANNEL_COUNT; ++i)
    {
        const ChannelInfo* found = find_channel(CHANNEL_TABLE[i].name);
        check(found == &CHANNEL_TABLE[i], std::string("lookup of ") + std::string(CHANNEL_TABLE[i].name));
        check(CHANNEL_TABLE[i].slots >= 2, "at least two slots");
        check(CHANNEL_TABLE[i].payload > 0, "payload is not empty");
    }

    check(find_channel("raccoon/gyro/value") == nullptr, "an old channel name is not in the table");
    check(find_channel("") == nullptr, "an empty name is not in the table");

    // Fixed-size types take their slot size from the type.
    const ChannelInfo* power = find_channel("hardware/motor/2/cmd/power");
    check(power != nullptr && power->type == "scalar_i32_t" && power->fixed_size && power->payload == 12,
          "motor power is a 12 byte scalar_i32_t");
    check(power != nullptr && power->reliable && !power->retained, "motor power is reliable and not retained");
    check(slot_stride(12) == 64 && slot_stride(48) == 64 && slot_stride(49) == 128, "slot stride is a cache line multiple");

    const ChannelInfo* gyro = find_channel(HARDWARE_IMU_STATE_GYRO);
    check(gyro != nullptr && gyro->payload == 20 && gyro->retained && !gyro->reliable, "gyro is a retained 20 byte vector");

    // Types without a fixed size carry the limit of the table.
    const ChannelInfo* render = find_channel(UI_SCREEN_RENDER);
    check(render != nullptr && !render->fixed_size && render->payload == 32u * 1024 * 1024 && render->slots == 3,
          "the screen render is limited to 32 MiB in 3 slots");
    const ChannelInfo* error = find_channel(MESSAGES_ERROR);
    check(error != nullptr && error->type == "string_t" && !error->retained, "messages/error is an event");

    // The latch and the chassis stop are reliable, the chassis velocity stream is not.
    check(find_channel(HARDWARE_LATCH_CMD_ENGAGED)->reliable, "latch is reliable");
    check(find_channel(HARDWARE_CHASSIS_CMD_STOP)->reliable, "chassis stop is reliable");
    check(!find_channel(HARDWARE_CHASSIS_CMD_VELOCITY)->reliable, "chassis velocity is a stream");

    // Port functions return the table name, or an empty name for a port that does not exist.
    for (int port = 0; port < 4; ++port)
    {
        const std::string expected = "hardware/motor/" + std::to_string(port) + "/cmd/power";
        check(hardware_motor_cmd_power(port) == expected, "motor power name of port " + std::to_string(port));
        check(find_channel(hardware_motor_cmd_power(port)) != nullptr, "motor power port is in the table");
    }
    check(hardware_motor_cmd_power(4).empty() && hardware_motor_cmd_power(-1).empty(), "ports outside the range have no name");
    check(hardware_digital_state_value(10) == "hardware/digital/10/state/value", "digital has ports 0 to 10");
    check(hardware_digital_state_value(11).empty(), "digital has no port 11");

    std::printf("channel table: %d failures\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
