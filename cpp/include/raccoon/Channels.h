#pragma once

#include <string>

namespace raccoon::Channels
{
    // Sensor data
    constexpr auto GYRO = "raccoon/gyro/value";
    constexpr auto ACCELEROMETER = "raccoon/accel/value";
    constexpr auto LINEAR_ACCELERATION = "raccoon/linear_accel/value";
    constexpr auto ACCEL_VELOCITY = "raccoon/accel_velocity/value";
    constexpr auto MAGNETOMETER = "raccoon/mag/value";
    constexpr auto DMP_ORIENTATION = "raccoon/imu/quaternion";
    constexpr auto HEADING = "raccoon/imu/heading";
    constexpr auto TEMPERATURE = "raccoon/imu/temp/value";
    constexpr auto BATTERY_VOLTAGE = "raccoon/battery/voltage";
    constexpr auto GYRO_ACCURACY = "raccoon/gyro/accuracy";
    constexpr auto ACCEL_ACCURACY = "raccoon/accel/accuracy";
    constexpr auto COMPASS_ACCURACY = "raccoon/mag/accuracy";
    constexpr auto QUATERNION_ACCURACY = "raccoon/imu/quaternion_accuracy";
    constexpr auto CPU_TEMPERATURE = "raccoon/cpu/temp/value";

    // Odometry (computed on STM32)
    constexpr auto ODOM_POS_X = "raccoon/odometry/pos_x";
    constexpr auto ODOM_POS_Y = "raccoon/odometry/pos_y";
    constexpr auto ODOM_HEADING = "raccoon/odometry/heading";
    constexpr auto ODOM_VX = "raccoon/odometry/vx";
    constexpr auto ODOM_VY = "raccoon/odometry/vy";
    constexpr auto ODOM_WZ = "raccoon/odometry/wz";

    // Odometry commands (from library to Pi reader)
    constexpr auto KINEMATICS_CONFIG_CMD = "raccoon/kinematics/config_cmd";
    constexpr auto ODOM_RESET_CMD = "raccoon/odometry/reset_cmd";

    // Screen
    constexpr auto SCREEN_RENDER = "raccoon/screen_render";
    constexpr auto SCREEN_RENDER_ANSWER = "raccoon/screen_render/answer";

    // Vision
    constexpr auto YOLO_FRAME = "raccoon/yolo/frame";

    // Camera
    constexpr auto CAM_DETECTIONS = "raccoon/cam/detections";

    // System
    constexpr auto ERROR_MESSAGES = "raccoon/errors";
    constexpr auto SHUTDOWN_CMD = "raccoon/system/shutdown_cmd";
    constexpr auto SHUTDOWN_STATUS = "raccoon/system/shutdown_status";

    // Parametric channels (port-indexed)
    using PortId = int;

    inline std::string servoMode(const PortId port)
    {
        return "raccoon/servo/" + std::to_string(port) + "/mode";
    }

    inline std::string servoPosition(const PortId port)
    {
        return "raccoon/servo/" + std::to_string(port) + "/position";
    }

    inline std::string servoPositionCommand(const PortId port)
    {
        return "raccoon/servo/" + std::to_string(port) + "/position_cmd";
    }

    inline std::string servoSmoothPositionCommand(const PortId port)
    {
        return "raccoon/servo/" + std::to_string(port) + "/smooth_cmd";
    }

    inline std::string backEmf(const PortId port)
    {
        return "raccoon/bemf/" + std::to_string(port) + "/value";
    }

    inline std::string analog(const PortId port)
    {
        return "raccoon/analog/" + std::to_string(port) + "/value";
    }

    inline std::string digital(const PortId bit)
    {
        return "raccoon/digital/" + std::to_string(bit) + "/value";
    }

    inline std::string motorPowerCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/power_cmd";
    }

    inline std::string motorModeCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/mode_cmd";
    }

    inline std::string motorStopCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/stop_cmd";
    }

    inline std::string motorVelocityCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/velocity_cmd";
    }

    inline std::string motorPositionCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/position_cmd";
    }

    inline std::string motorRelativeCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/relative_cmd";
    }

    inline std::string motorPidCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/pid_cmd";
    }

    inline std::string motorPositionResetCommand(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/position_reset_cmd";
    }

    inline std::string motorPower(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/power";
    }

    inline std::string motorPosition(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/position";
    }

    inline std::string motorDone(const PortId port)
    {
        return "raccoon/motor/" + std::to_string(port) + "/done";
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