#pragma once

#include <array>
#include <string>

namespace raccoon::Channels
{
    // Per-port channel-name accessors are hot — at 200 Hz × 4 motors the
    // old `"raccoon/motor/" + to_string(port) + "/velocity_cmd"` built and
    // freed ~800 std::strings/sec just to publish (perf trace showed ~8 %
    // CPU in malloc ← TransportWriter::setMotorVelocity).
    //
    // Each accessor now memoises into a static `std::array<std::string, 16>`
    // and returns a reference to the cached string. 16 covers the existing
    // hardware port counts (4 motors + 4 servos + 6 analog + 11 digital —
    // digital tops at 15 in the Wombat schema). Callers must NOT keep the
    // reference longer than they keep the cache alive, which is forever
    // since storage is static.
    #define RACCOON_CACHED_CHANNEL(prefix, suffix)                              \
        do                                                                       \
        {                                                                        \
            static const std::array<std::string, 16> _cache = []                 \
            {                                                                    \
                std::array<std::string, 16> a;                                   \
                for (int _i = 0; _i < 16; ++_i)                                  \
                {                                                                \
                    a[_i] = std::string(prefix) + std::to_string(_i) + (suffix); \
                }                                                                \
                return a;                                                        \
            }();                                                                 \
            return _cache[(port < 16) ? port : 0];                               \
        } while (false)


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
    constexpr auto HEARTBEAT_CMD = "raccoon/system/heartbeat_cmd";

    // Feature flags (runtime opt-in toggles)
    constexpr auto BEMF_ENABLED_CMD = "raccoon/cmd/feature/bemf_enabled";
    constexpr auto BEMF_ENABLED = "raccoon/feature/bemf_enabled";

    // Chassis velocity command (body frame [vx (m/s), vy (m/s), wz (rad/s)],
    // vector3f_t). Drives MOT_MODE_CHASSIS on the STM32: the firmware maps this
    // to per-wheel setpoints via forward kinematics and runs the per-motor PID,
    // so the full chassis velocity loop closes on-MCU. Global (not port-indexed).
    constexpr auto CHASSIS_VELOCITY_CMD = "raccoon/chassis/velocity_cmd";

    // Parametric channels (port-indexed)
    using PortId = int;

    // Servo mode STATE — reader publishes the current mode here; UIs
    // subscribe for state feedback. Mirrors the motorPower / motorPower
    // Command split: state lives on `mode`, commands on `mode_cmd`.
    // Splitting the formerly-shared single `mode` channel removed the
    // reader's self-loopback (it used to subscribe to its own publishes
    // for ~5 ms of internal latency floor) and aligns servo channels
    // with the established motor convention.
    inline const std::string& servoMode(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/servo/", "/mode");
    }

    // Servo mode COMMAND — publishers (raccoon-lib LcmDataWriter,
    // botui's disable buttons) write desired mode here; reader's
    // CommandSubscriber listens. Separate from the state channel above
    // so reader does not subscribe to its own publishes.
    inline const std::string& servoModeCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/servo/", "/mode_cmd");
    }

    inline const std::string& servoPosition(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/servo/", "/position");
    }

    inline const std::string& servoPositionCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/servo/", "/position_cmd");
    }

    inline const std::string& servoSmoothPositionCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/servo/", "/smooth_cmd");
    }

    inline const std::string& backEmf(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/bemf/", "/value");
    }

    inline const std::string& analog(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/analog/", "/value");
    }

    inline const std::string& digital(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/digital/", "/value");
    }

    inline const std::string& motorPowerCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/power_cmd");
    }

    inline const std::string& motorModeCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/mode_cmd");
    }

    inline const std::string& motorStopCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/stop_cmd");
    }

    inline const std::string& motorVelocityCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/velocity_cmd");
    }

    inline const std::string& motorPositionCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/position_cmd");
    }

    inline const std::string& motorRelativeCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/relative_cmd");
    }

    inline const std::string& motorPidCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/pid_cmd");
    }

    inline const std::string& motorPositionResetCommand(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/position_reset_cmd");
    }

    inline const std::string& motorPower(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/power");
    }

    inline const std::string& motorPosition(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/position");
    }

    inline const std::string& motorDone(const PortId port)
    {
        RACCOON_CACHED_CHANNEL("raccoon/motor/", "/done");
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