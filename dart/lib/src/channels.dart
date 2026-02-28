/// Channel name definitions for LCM communication
class Channels {
  Channels._();

  // Sensor data
  static const gyro = 'libstp/gyro/value';
  static const accelerometer = 'libstp/accel/value';
  static const linearAcceleration = 'libstp/linear_accel/value';
  static const accelVelocity = 'libstp/accel_velocity/value';
  static const magnetometer = 'libstp/mag/value';
  static const orientation = 'libstp/imu/quaternion';
  static const heading = 'libstp/imu/heading';
  static const temperature = 'libstp/imu/temp/value';
  static const batteryVoltage = 'libstp/battery/voltage';
  static const gyroAccuracy = 'libstp/gyro/accuracy';
  static const accelAccuracy = 'libstp/accel/accuracy';
  static const compassAccuracy = 'libstp/mag/accuracy';
  static const quaternionAccuracy = 'libstp/imu/quaternion_accuracy';
  static const cpuTemperature = 'libstp/cpu/temp/value';

  // IMU orientation commands
  static const imuGyroOrientationCmd = 'libstp/imu/gyro_orientation_cmd';
  static const imuCompassOrientationCmd = 'libstp/imu/compass_orientation_cmd';
  static const axisRemapCmd = 'libstp/imu/axis_remap_cmd';

  // BEMF
  static const bemfNominalVoltageCmd = 'libstp/bemf/nominal_voltage_cmd';

  // System
  static const dataDumpRequest = 'libstp/system/dump_request';
  static const errorMessages = 'libstp/errors';
  static const shutdownCmd = 'libstp/system/shutdown_cmd';
  static const shutdownStatus = 'libstp/system/shutdown_status';

  // Parametric channels
  static String servoMode(int port) => 'libstp/servo/$port/mode';
  static String servoPosition(int port) => 'libstp/servo/$port/position';
  static String servoPositionCommand(int port) => 'libstp/servo/$port/position_cmd';
  static String backEmf(int port) => 'libstp/bemf/$port/value';
  static String bemfScaleCommand(int port) => 'libstp/bemf/$port/scale_cmd';
  static String bemfOffsetCommand(int port) => 'libstp/bemf/$port/offset_cmd';
  static String analog(int port) => 'libstp/analog/$port/value';
  static String digital(int bit) => 'libstp/digital/$bit/value';
  static String motorPowerCommand(int port) => 'libstp/motor/$port/power_cmd';
  static String motorStopCommand(int port) => 'libstp/motor/$port/stop_cmd';
  static String motorVelocityCommand(int port) => 'libstp/motor/$port/velocity_cmd';
  static String motorPositionCommand(int port) => 'libstp/motor/$port/position_cmd';
  static String motorPidCommand(int port) => 'libstp/motor/$port/pid_cmd';
  static String motorPositionResetCommand(int port) => 'libstp/motor/$port/position_reset_cmd';
  static String motorPower(int port) => 'libstp/motor/$port/power';
  static String motorPosition(int port) => 'libstp/motor/$port/position';
  static String motorDone(int port) => 'libstp/motor/$port/done';
}

/// Internal protocol channels
class ProtocolChannels {
  ProtocolChannels._();

  static const ack = '__raccoon/ack';
  static const retainRequest = '__raccoon/retain_request';
  static String reliableChannel(String channel) => '__raccoon/r/$channel';
}
