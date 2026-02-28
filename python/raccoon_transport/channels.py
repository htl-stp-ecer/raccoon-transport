"""Channel name definitions for LCM communication."""


class Channels:
    """All channel name constants and factory methods."""

    # Sensor data
    GYRO = "libstp/gyro/value"
    ACCELEROMETER = "libstp/accel/value"
    LINEAR_ACCELERATION = "libstp/linear_accel/value"
    ACCEL_VELOCITY = "libstp/accel_velocity/value"
    MAGNETOMETER = "libstp/mag/value"
    ORIENTATION = "libstp/imu/quaternion"
    HEADING = "libstp/imu/heading"
    TEMPERATURE = "libstp/imu/temp/value"
    BATTERY_VOLTAGE = "libstp/battery/voltage"
    GYRO_ACCURACY = "libstp/gyro/accuracy"
    ACCEL_ACCURACY = "libstp/accel/accuracy"
    COMPASS_ACCURACY = "libstp/mag/accuracy"
    QUATERNION_ACCURACY = "libstp/imu/quaternion_accuracy"
    CPU_TEMPERATURE = "libstp/cpu/temp/value"

    # IMU orientation commands
    IMU_GYRO_ORIENTATION_CMD = "libstp/imu/gyro_orientation_cmd"
    IMU_COMPASS_ORIENTATION_CMD = "libstp/imu/compass_orientation_cmd"
    AXIS_REMAP_CMD = "libstp/imu/axis_remap_cmd"

    # BEMF
    BEMF_NOMINAL_VOLTAGE_CMD = "libstp/bemf/nominal_voltage_cmd"

    # System
    DATA_DUMP_REQUEST = "libstp/system/dump_request"
    ERROR_MESSAGES = "libstp/errors"
    SHUTDOWN_CMD = "libstp/system/shutdown_cmd"
    SHUTDOWN_STATUS = "libstp/system/shutdown_status"

    # Parametric channels
    @staticmethod
    def servo_mode(port: int) -> str:
        return f"libstp/servo/{port}/mode"

    @staticmethod
    def servo_position(port: int) -> str:
        return f"libstp/servo/{port}/position"

    @staticmethod
    def servo_position_command(port: int) -> str:
        return f"libstp/servo/{port}/position_cmd"

    @staticmethod
    def back_emf(port: int) -> str:
        return f"libstp/bemf/{port}/value"

    @staticmethod
    def bemf_scale_command(port: int) -> str:
        return f"libstp/bemf/{port}/scale_cmd"

    @staticmethod
    def bemf_offset_command(port: int) -> str:
        return f"libstp/bemf/{port}/offset_cmd"

    @staticmethod
    def analog(port: int) -> str:
        return f"libstp/analog/{port}/value"

    @staticmethod
    def digital(bit: int) -> str:
        return f"libstp/digital/{bit}/value"

    @staticmethod
    def motor_power_command(port: int) -> str:
        return f"libstp/motor/{port}/power_cmd"

    @staticmethod
    def motor_stop_command(port: int) -> str:
        return f"libstp/motor/{port}/stop_cmd"

    @staticmethod
    def motor_velocity_command(port: int) -> str:
        return f"libstp/motor/{port}/velocity_cmd"

    @staticmethod
    def motor_position_command(port: int) -> str:
        return f"libstp/motor/{port}/position_cmd"

    @staticmethod
    def motor_pid_command(port: int) -> str:
        return f"libstp/motor/{port}/pid_cmd"

    @staticmethod
    def motor_position_reset_command(port: int) -> str:
        return f"libstp/motor/{port}/position_reset_cmd"

    @staticmethod
    def motor_power(port: int) -> str:
        return f"libstp/motor/{port}/power"

    @staticmethod
    def motor_position(port: int) -> str:
        return f"libstp/motor/{port}/position"

    @staticmethod
    def motor_done(port: int) -> str:
        return f"libstp/motor/{port}/done"


class ProtocolChannels:
    """Internal protocol channels for reliability/retain."""

    ACK = "__raccoon/ack"
    RETAIN_REQUEST = "__raccoon/retain_request"

    @staticmethod
    def reliable_channel(channel: str) -> str:
        return f"__raccoon/r/{channel}"
