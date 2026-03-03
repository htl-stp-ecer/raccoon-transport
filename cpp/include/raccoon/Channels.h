#pragma once

#include <string>

namespace raccoon::Channels
{
    // Sensor data
    constexpr auto GYRO = "libstp/gyro/value";
    constexpr auto ACCELEROMETER = "libstp/accel/value";
    constexpr auto LINEAR_ACCELERATION = "libstp/linear_accel/value";
    constexpr auto ACCEL_VELOCITY = "libstp/accel_velocity/value";
    constexpr auto MAGNETOMETER = "libstp/mag/value";
    constexpr auto ORIENTATION = "libstp/imu/quaternion";
    constexpr auto HEADING = "libstp/imu/heading";
    constexpr auto TEMPERATURE = "libstp/imu/temp/value";
    constexpr auto BATTERY_VOLTAGE = "libstp/battery/voltage";
    constexpr auto GYRO_ACCURACY = "libstp/gyro/accuracy";
    constexpr auto ACCEL_ACCURACY = "libstp/accel/accuracy";
    constexpr auto COMPASS_ACCURACY = "libstp/mag/accuracy";
    constexpr auto QUATERNION_ACCURACY = "libstp/imu/quaternion_accuracy";
    constexpr auto CPU_TEMPERATURE = "libstp/cpu/temp/value";

    // Screen
    constexpr auto SCREEN_RENDER = "libstp/screen_render";

    // System
    constexpr auto ERROR_MESSAGES = "libstp/errors";
    constexpr auto SHUTDOWN_CMD = "libstp/system/shutdown_cmd";
    constexpr auto SHUTDOWN_STATUS = "libstp/system/shutdown_status";

    // Parametric channels (port-indexed)
    using PortId = int;

    inline std::string servoMode(const PortId port)
    {
        return "libstp/servo/" + std::to_string(port) + "/mode";
    }

    inline std::string servoPosition(const PortId port)
    {
        return "libstp/servo/" + std::to_string(port) + "/position";
    }

    inline std::string servoPositionCommand(const PortId port)
    {
        return "libstp/servo/" + std::to_string(port) + "/position_cmd";
    }

    inline std::string backEmf(const PortId port)
    {
        return "libstp/bemf/" + std::to_string(port) + "/value";
    }

    inline std::string bemfScaleCommand(const PortId port)
    {
        return "libstp/bemf/" + std::to_string(port) + "/scale_cmd";
    }

    inline std::string bemfOffsetCommand(const PortId port)
    {
        return "libstp/bemf/" + std::to_string(port) + "/offset_cmd";
    }

    inline std::string analog(const PortId port)
    {
        return "libstp/analog/" + std::to_string(port) + "/value";
    }

    inline std::string digital(const PortId bit)
    {
        return "libstp/digital/" + std::to_string(bit) + "/value";
    }

    inline std::string motorPowerCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/power_cmd";
    }

    inline std::string motorStopCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/stop_cmd";
    }

    inline std::string motorVelocityCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/velocity_cmd";
    }

    inline std::string motorPositionCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/position_cmd";
    }

    inline std::string motorRelativeCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/relative_cmd";
    }

    inline std::string motorPidCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/pid_cmd";
    }

    inline std::string motorPositionResetCommand(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/position_reset_cmd";
    }

    inline std::string motorPower(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/power";
    }

    inline std::string motorPosition(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/position";
    }

    inline std::string motorDone(const PortId port)
    {
        return "libstp/motor/" + std::to_string(port) + "/done";
    }

    // Protocol channels (internal)
    namespace Protocol
    {
        constexpr auto ACK = "__raccoon/ack";
        constexpr auto RETAIN_REQUEST = "__raccoon/retain_request";

        inline std::string reliableChannel(const std::string& channel)
        {
            return "__raccoon/r/" + channel;
        }
    }
}
