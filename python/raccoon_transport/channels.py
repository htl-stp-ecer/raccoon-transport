"""Channel-name helpers shared by the Python transport implementation."""


class Channels:
    """Stable application-level channel names used by the Python transport."""

    # Sensor data
    GYRO = "raccoon/gyro/value"
    ACCELEROMETER = "raccoon/accel/value"
    LINEAR_ACCELERATION = "raccoon/linear_accel/value"
    ACCEL_VELOCITY = "raccoon/accel_velocity/value"
    MAGNETOMETER = "raccoon/mag/value"
    ORIENTATION = "raccoon/imu/quaternion"
    HEADING = "raccoon/imu/heading"
    TEMPERATURE = "raccoon/imu/temp/value"
    BATTERY_VOLTAGE = "raccoon/battery/voltage"
    GYRO_ACCURACY = "raccoon/gyro/accuracy"
    ACCEL_ACCURACY = "raccoon/accel/accuracy"
    COMPASS_ACCURACY = "raccoon/mag/accuracy"
    QUATERNION_ACCURACY = "raccoon/imu/quaternion_accuracy"
    CPU_TEMPERATURE = "raccoon/cpu/temp/value"

    # Screen
    SCREEN_RENDER = "raccoon/screen_render"
    SCREEN_RENDER_ANSWER = "raccoon/screen_render/answer"

    # Vision
    YOLO_FRAME = "raccoon/yolo/frame"

    # System
    ERROR_MESSAGES = "raccoon/errors"
    SHUTDOWN_CMD = "raccoon/system/shutdown_cmd"
    SHUTDOWN_STATUS = "raccoon/system/shutdown_status"
    HEARTBEAT_CMD = "raccoon/system/heartbeat_cmd"

    # Parametric channels
    @staticmethod
    def servo_mode(port: int) -> str:
        return f"raccoon/servo/{port}/mode"

    @staticmethod
    def servo_mode_command(port: int) -> str:
        # Commands go here, state on servo_mode — split so reader does
        # not subscribe to its own publishes (eliminates the ~5 ms
        # self-loopback delay floor on inbound LCM latency).
        return f"raccoon/servo/{port}/mode_cmd"

    @staticmethod
    def servo_position(port: int) -> str:
        return f"raccoon/servo/{port}/position"

    @staticmethod
    def servo_position_command(port: int) -> str:
        return f"raccoon/servo/{port}/position_cmd"

    @staticmethod
    def back_emf(port: int) -> str:
        return f"raccoon/bemf/{port}/value"

    @staticmethod
    def bemf_scale_command(port: int) -> str:
        return f"raccoon/bemf/{port}/scale_cmd"

    @staticmethod
    def bemf_offset_command(port: int) -> str:
        return f"raccoon/bemf/{port}/offset_cmd"

    @staticmethod
    def analog(port: int) -> str:
        return f"raccoon/analog/{port}/value"

    @staticmethod
    def digital(bit: int) -> str:
        return f"raccoon/digital/{bit}/value"

    @staticmethod
    def motor_power_command(port: int) -> str:
        return f"raccoon/motor/{port}/power_cmd"

    @staticmethod
    def motor_mode_command(port: int) -> str:
        return f"raccoon/motor/{port}/mode_cmd"

    @staticmethod
    def motor_stop_command(port: int) -> str:
        return f"raccoon/motor/{port}/stop_cmd"

    @staticmethod
    def motor_velocity_command(port: int) -> str:
        return f"raccoon/motor/{port}/velocity_cmd"

    @staticmethod
    def motor_position_command(port: int) -> str:
        return f"raccoon/motor/{port}/position_cmd"

    @staticmethod
    def motor_relative_command(port: int) -> str:
        return f"raccoon/motor/{port}/relative_cmd"

    @staticmethod
    def motor_pid_command(port: int) -> str:
        return f"raccoon/motor/{port}/pid_cmd"

    @staticmethod
    def motor_position_reset_command(port: int) -> str:
        return f"raccoon/motor/{port}/position_reset_cmd"

    @staticmethod
    def motor_power(port: int) -> str:
        return f"raccoon/motor/{port}/power"

    @staticmethod
    def motor_position(port: int) -> str:
        return f"raccoon/motor/{port}/position"

    @staticmethod
    def motor_done(port: int) -> str:
        return f"raccoon/motor/{port}/done"


class ProtocolChannels:
    """Internal protocol channels used for reliable and retained delivery."""

    ACK = "__raccoon/ack"
    RETAIN_REQUEST = "__raccoon/retain_request"

    @staticmethod
    def reliable_channel(channel: str) -> str:
        """Return the wrapped channel name used by the reliability layer."""
        return f"__raccoon/r/{channel}"
