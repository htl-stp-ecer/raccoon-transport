import 'package:raccoon_transport/raccoon_transport.dart';
import 'package:test/test.dart';

void main() {
  test('a channel is named by its path', () {
    expect(channels.hardware.imu.state.gyro, 'hardware/imu/state/gyro');
    expect(channels.hardware.latch.cmd.engaged, 'hardware/latch/cmd/engaged');
    expect(channels.messages.error, 'messages/error');
    expect(channels.camera.state.frame, 'camera/state/frame');
    expect(channels.hardware.calibBoard.icm.state.gyroCorrected,
        'hardware/calib_board/icm/state/gyro_corrected');
  });

  test('a channel of a port range is a function of the port', () {
    expect(channels.hardware.motor.cmd.power(2), 'hardware/motor/2/cmd/power');
    expect(channels.hardware.digital.state.value(10),
        'hardware/digital/10/state/value');
  });

  test('a port outside the range is an error', () {
    expect(() => channels.hardware.motor.cmd.power(4), throwsRangeError);
    expect(() => channels.hardware.motor.cmd.power(-1), throwsRangeError);
    expect(() => channels.hardware.digital.state.value(11), throwsRangeError);
  });
}
