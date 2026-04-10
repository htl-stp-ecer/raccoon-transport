/// Channel name definitions for LCM communication
class Channels {
  Channels._();

  // Sensor data
  static const gyro = 'raccoon/gyro/value';
  static const accelerometer = 'raccoon/accel/value';
  static const linearAcceleration = 'raccoon/linear_accel/value';
  static const accelVelocity = 'raccoon/accel_velocity/value';
  static const magnetometer = 'raccoon/mag/value';
  static const orientation = 'raccoon/imu/quaternion';
  static const heading = 'raccoon/imu/heading';
  static const temperature = 'raccoon/imu/temp/value';
  static const batteryVoltage = 'raccoon/battery/voltage';
  static const gyroAccuracy = 'raccoon/gyro/accuracy';
  static const accelAccuracy = 'raccoon/accel/accuracy';
  static const compassAccuracy = 'raccoon/mag/accuracy';
  static const quaternionAccuracy = 'raccoon/imu/quaternion_accuracy';
  static const cpuTemperature = 'raccoon/cpu/temp/value';

  // Screen
  static const screenRender = 'raccoon/screen_render';
  static const screenRenderAnswer = 'raccoon/screen_render/answer';

  // Camera
  static const camDetections = 'raccoon/cam/detections';
  static const camFrame = 'raccoon/cam/frame';
  static const camStreamCtl = 'raccoon/cam/stream_ctl';
  static const camConfig = 'raccoon/cam/config';

  // System
  static const errorMessages = 'raccoon/errors';
  static const shutdownCmd = 'raccoon/system/shutdown_cmd';
  static const shutdownStatus = 'raccoon/system/shutdown_status';

  // Parametric channels
  static String servoMode(int port) => 'raccoon/servo/$port/mode';
  static String servoPosition(int port) => 'raccoon/servo/$port/position';
  static String servoPositionCommand(int port) => 'raccoon/servo/$port/position_cmd';
  static String backEmf(int port) => 'raccoon/bemf/$port/value';
  static String analog(int port) => 'raccoon/analog/$port/value';
  static String digital(int bit) => 'raccoon/digital/$bit/value';
  static String motorPowerCommand(int port) => 'raccoon/motor/$port/power_cmd';
  static String motorModeCommand(int port) => 'raccoon/motor/$port/mode_cmd';
  static String motorStopCommand(int port) => 'raccoon/motor/$port/stop_cmd';
  static String motorVelocityCommand(int port) => 'raccoon/motor/$port/velocity_cmd';
  static String motorPositionCommand(int port) => 'raccoon/motor/$port/position_cmd';
  static String motorRelativeCommand(int port) => 'raccoon/motor/$port/relative_cmd';
  static String motorPidCommand(int port) => 'raccoon/motor/$port/pid_cmd';
  static String motorPositionResetCommand(int port) => 'raccoon/motor/$port/position_reset_cmd';
  static String motorPower(int port) => 'raccoon/motor/$port/power';
  static String motorPosition(int port) => 'raccoon/motor/$port/position';
  static String motorDone(int port) => 'raccoon/motor/$port/done';
}

/// Internal protocol channels
class ProtocolChannels {
  ProtocolChannels._();

  static const ack = '__raccoon/ack';
  static const retainRequest = '__raccoon/retain_request';
  static String reliableChannel(String channel) => '__raccoon/r/$channel';
}
