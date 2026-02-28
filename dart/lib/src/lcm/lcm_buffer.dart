import 'dart:typed_data';

/// Interface for LCM messages
abstract class LcmMessage {
  /// Get the LCM fingerprint for this message type
  int get lcmFingerprint;

  /// Encode this message to the given buffer (with fingerprint header)
  void encode(LcmBuffer buf);

  /// Encode only the fields (without fingerprint header), used for nested types
  void encodeBody(LcmBuffer buf);
}

/// Buffer for encoding and decoding LCM messages
class LcmBuffer {
  final ByteData _data;
  int _position = 0;

  LcmBuffer(int size) : _data = ByteData(size);

  LcmBuffer.fromUint8List(Uint8List data)
      : _data = ByteData.view(data.buffer, data.offsetInBytes, data.lengthInBytes);

  /// Get current position in the buffer
  int get position => _position;

  /// Set current position in the buffer
  set position(int value) => _position = value;

  /// Get the underlying Uint8List
  Uint8List get uint8List => _data.buffer.asUint8List(_data.offsetInBytes, _data.lengthInBytes);

  // Encoding methods (big-endian)

  void putInt8(int value) {
    _data.setInt8(_position, value);
    _position += 1;
  }

  void putUint8(int value) {
    _data.setUint8(_position, value);
    _position += 1;
  }

  void putInt16(int value) {
    _data.setInt16(_position, value, Endian.big);
    _position += 2;
  }

  void putUint16(int value) {
    _data.setUint16(_position, value, Endian.big);
    _position += 2;
  }

  void putInt32(int value) {
    _data.setInt32(_position, value, Endian.big);
    _position += 4;
  }

  void putUint32(int value) {
    _data.setUint32(_position, value, Endian.big);
    _position += 4;
  }

  void putInt64(int value) {
    _data.setInt64(_position, value, Endian.big);
    _position += 8;
  }

  void putUint64(int value) {
    _data.setUint64(_position, value, Endian.big);
    _position += 8;
  }

  void putFloat32(double value) {
    _data.setFloat32(_position, value, Endian.big);
    _position += 4;
  }

  void putFloat64(double value) {
    _data.setFloat64(_position, value, Endian.big);
    _position += 8;
  }

  void putUint8List(List<int> bytes) {
    // Use efficient bulk copy when possible
    final target = _data.buffer.asUint8List(_data.offsetInBytes + _position, bytes.length);
    if (bytes is Uint8List) {
      target.setAll(0, bytes);
    } else {
      target.setRange(0, bytes.length, bytes);
    }
    _position += bytes.length;
  }

  // Decoding methods (big-endian)

  int getInt8() {
    final value = _data.getInt8(_position);
    _position += 1;
    return value;
  }

  int getUint8() {
    final value = _data.getUint8(_position);
    _position += 1;
    return value;
  }

  int getInt16() {
    final value = _data.getInt16(_position, Endian.big);
    _position += 2;
    return value;
  }

  int getUint16() {
    final value = _data.getUint16(_position, Endian.big);
    _position += 2;
    return value;
  }

  int getInt32() {
    final value = _data.getInt32(_position, Endian.big);
    _position += 4;
    return value;
  }

  int getUint32() {
    final value = _data.getUint32(_position, Endian.big);
    _position += 4;
    return value;
  }

  int getInt64() {
    final value = _data.getInt64(_position, Endian.big);
    _position += 8;
    return value;
  }

  int getUint64() {
    final value = _data.getUint64(_position, Endian.big);
    _position += 8;
    return value;
  }

  double getFloat32() {
    final value = _data.getFloat32(_position, Endian.big);
    _position += 4;
    return value;
  }

  double getFloat64() {
    final value = _data.getFloat64(_position, Endian.big);
    _position += 8;
    return value;
  }

  Uint8List getUint8List(int length) {
    // Use efficient view/copy instead of byte-by-byte
    final source = _data.buffer.asUint8List(_data.offsetInBytes + _position, length);
    final result = Uint8List.fromList(source); // Creates a copy
    _position += length;
    return result;
  }
}
