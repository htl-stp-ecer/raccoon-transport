import 'dart:async';
import 'dart:io';
import 'dart:typed_data';
import 'lcm_buffer.dart';

/// LCM protocol constants
const int _lcmMagicShort = 0x4c433032; // ASCII "LC02"
const int _lcmMagicLong = 0x4c433033; // ASCII "LC03"
const int _lcmShortMessageMaxSize = 65499;
const int _lcmFragmentMaxPayload = 65487;
const int _maxChannelNameLength = 63;

/// Default LCM multicast address and port
const String _defaultNetwork = '239.255.76.67';
const int _defaultPort = 7667;
const int _defaultTtl = 0;

/// Callback type for message handlers
typedef LcmMessageHandler = void Function(String channel, Uint8List data);

/// Callback type for debug logging
typedef LcmDebugLogger = void Function(String message);

/// Performance statistics for LCM
class LcmStats {
  int messagesSent = 0;
  int messagesReceived = 0;
  int bytesSent = 0;
  int bytesReceived = 0;
  int fragmentsSent = 0;
  int fragmentsReceived = 0;
  int fragmentsDropped = 0;
  int dispatchErrors = 0;

  // Latency tracking (in microseconds)
  final List<int> _recentLatencies = [];
  static const int _maxLatencySamples = 1000;

  // Timing
  final Stopwatch _uptime = Stopwatch();
  DateTime? _startTime;
  int _lastReceiveTimestamp = 0;
  int _lastSendTimestamp = 0;

  void _start() {
    _startTime = DateTime.now();
    _uptime.start();
  }

  void _recordLatency(int microseconds) {
    _recentLatencies.add(microseconds);
    if (_recentLatencies.length > _maxLatencySamples) {
      _recentLatencies.removeAt(0);
    }
  }

  /// Get latency statistics (min, median, avg, max) in microseconds
  /// Returns null if no latency data available
  Map<String, int>? get latencyStats {
    if (_recentLatencies.isEmpty) return null;

    final sorted = List<int>.from(_recentLatencies)..sort();
    final sum = sorted.reduce((a, b) => a + b);

    return {
      'min': sorted.first,
      'median': sorted[sorted.length ~/ 2],
      'avg': sum ~/ sorted.length,
      'p99': sorted[(sorted.length * 0.99).toInt().clamp(0, sorted.length - 1)],
      'max': sorted.last,
      'samples': sorted.length,
    };
  }

  /// Messages per second (receive rate)
  double get receiveRate {
    final seconds = _uptime.elapsedMicroseconds / 1000000;
    return seconds > 0 ? messagesReceived / seconds : 0;
  }

  /// Messages per second (send rate)
  double get sendRate {
    final seconds = _uptime.elapsedMicroseconds / 1000000;
    return seconds > 0 ? messagesSent / seconds : 0;
  }

  /// Uptime in seconds
  double get uptimeSeconds => _uptime.elapsedMicroseconds / 1000000;

  /// Reset all statistics
  void reset() {
    messagesSent = 0;
    messagesReceived = 0;
    bytesSent = 0;
    bytesReceived = 0;
    fragmentsSent = 0;
    fragmentsReceived = 0;
    fragmentsDropped = 0;
    dispatchErrors = 0;
    _recentLatencies.clear();
    _uptime.reset();
    _uptime.start();
    _startTime = DateTime.now();
  }

  /// Record a latency measurement (in microseconds)
  /// Call this from your message handler to track end-to-end latency
  void recordLatency(int microseconds) {
    _recordLatency(microseconds);
  }

  /// Get current timestamp in microseconds (for latency measurement)
  static int get nowMicroseconds => DateTime.now().microsecondsSinceEpoch;

  @override
  String toString() {
    final lat = latencyStats;
    final latStr = lat != null
        ? 'min=${lat['min']}us med=${lat['median']}us avg=${lat['avg']}us max=${lat['max']}us'
        : 'no data';
    return 'LcmStats(sent: $messagesSent @ ${sendRate.toStringAsFixed(1)}/s, '
        'recv: $messagesReceived @ ${receiveRate.toStringAsFixed(1)}/s, '
        'latency: $latStr, '
        'uptime: ${uptimeSeconds.toStringAsFixed(1)}s)';
  }
}

/// Subscription object that can be used to unsubscribe
class LcmSubscription {
  final String _channelPattern;
  final RegExp? _regex; // null for exact matches (faster)
  final LcmMessageHandler _handler;
  final bool _isExactMatch;

  LcmSubscription._(this._channelPattern, this._regex, this._handler, this._isExactMatch);

  bool matches(String channel) {
    if (_isExactMatch) {
      return channel == _channelPattern;
    }
    return _regex!.hasMatch(channel);
  }
}

/// Main LCM client class for publishing and subscribing to messages
/// over UDP multicast.
class Lcm {
  RawDatagramSocket? _sendSocket;
  RawDatagramSocket? _recvSocket;
  InternetAddress? _multicastAddress;
  int _port = _defaultPort;
  int _ttl = _defaultTtl;
  int _messageSequenceNumber = 0;
  bool _closed = false;

  final List<LcmSubscription> _subscriptions = [];
  final Map<int, _FragmentBuffer> _fragmentBuffers = {};

  // Debug logging
  bool _debugEnabled = false;
  LcmDebugLogger? _debugLogger;
  final LcmStats _stats = LcmStats();

  /// Enable or disable debug logging
  set debugEnabled(bool value) => _debugEnabled = value;
  bool get debugEnabled => _debugEnabled;

  /// Set a custom debug logger (defaults to stderr)
  set debugLogger(LcmDebugLogger? logger) => _debugLogger = logger;

  /// Get performance statistics
  LcmStats get stats => _stats;

  void _log(String message) {
    if (!_debugEnabled) return;
    final timestamp = DateTime.now().toIso8601String();
    final formatted = '[$timestamp] LCM: $message';
    if (_debugLogger != null) {
      _debugLogger!(formatted);
    } else {
      stderr.writeln(formatted);
    }
  }
  StreamSubscription<RawSocketEvent>? _recvSubscription;

  /// Create a new LCM instance with the given provider URL.
  ///
  /// The URL format is: `udpm://[network[:port]]?ttl=[ttl]`
  ///
  /// If no URL is provided, defaults to `udpm://239.255.76.67:7667?ttl=0`
  ///
  /// Examples:
  /// - `udpm://239.255.76.67:7667` - Standard LCM multicast
  /// - `udpm://239.255.76.67:7667?ttl=1` - LCM with TTL 1
  ///
  /// Note: TTL=0 means packets never leave localhost.
  /// TTL=1 means packets stay on local network.
  static Future<Lcm> create([String? provider]) async {
    final lcm = Lcm._();
    await lcm._initialize(provider);
    return lcm;
  }

  Lcm._();

  Future<void> _initialize(String? provider) async {
    _parseProvider(provider);

    _multicastAddress = InternetAddress(_defaultNetwork);

    // Create send socket - no special binding needed
    _sendSocket = await RawDatagramSocket.bind(
      InternetAddress.anyIPv4,
      0,
      reuseAddress: true,
    );
    _sendSocket!.multicastHops = _ttl;
    _sendSocket!.broadcastEnabled = true;
    _sendSocket!.multicastLoopback = true; // Enable loopback for local testing

    // Create receive socket with reuseAddress and reusePort for multicast
    // This allows multiple processes to receive on the same multicast group
    _recvSocket = await RawDatagramSocket.bind(
      InternetAddress.anyIPv4,
      _port,
      reuseAddress: true,
      reusePort: true,
    );
    _recvSocket!.multicastHops = _ttl;
    _recvSocket!.multicastLoopback = true;
    _recvSocket!.readEventsEnabled = true;

    // Join multicast group
    _recvSocket!.joinMulticast(_multicastAddress!);

    // Set up receive handler - use a synchronous-style handler for lower latency
    _recvSubscription = _recvSocket!.listen(_handleSocketEvent);

    // Start stats tracking
    _stats._start();

    _log('Initialized: multicast=${_multicastAddress!.address}:$_port ttl=$_ttl');
  }

  void _parseProvider(String? provider) {
    if (provider == null || provider.isEmpty) {
      return;
    }

    final uri = Uri.tryParse(provider);
    if (uri == null || uri.scheme != 'udpm') {
      throw ArgumentError('Invalid LCM provider URL. Expected format: udpm://[address[:port]]?ttl=[ttl]');
    }

    if (uri.host.isNotEmpty) {
      // Parse network address and port from host
      final hostParts = uri.host.split(':');
      _defaultNetwork; // Keep default if not specified
      if (hostParts.isNotEmpty && hostParts[0].isNotEmpty) {
        // Use provided network address
      }
      if (uri.hasPort) {
        _port = uri.port;
      }
    }

    // Parse query parameters
    final ttlStr = uri.queryParameters['ttl'];
    if (ttlStr != null) {
      _ttl = int.tryParse(ttlStr) ?? _defaultTtl;
      if (_ttl == 0) {
        stderr.writeln('LCM: TTL set to zero, traffic will not leave localhost.');
      } else if (_ttl > 1) {
        stderr.writeln('LCM: TTL set to > 1... That\'s almost never correct!');
      }
    }
  }

  /// Publish a message on the given channel.
  ///
  /// [channel] - The channel name (max 63 characters)
  /// [data] - The message data to publish
  ///
  /// Returns the number of bytes sent, or throws an exception on error.
  int publish(String channel, Uint8List data) {
    if (_closed) {
      throw StateError('LCM instance has been closed');
    }

    if (channel.length > _maxChannelNameLength) {
      throw ArgumentError('Channel name too long: ${channel.length} > $_maxChannelNameLength');
    }

    _stats._lastSendTimestamp = DateTime.now().microsecondsSinceEpoch;

    final channelBytes = channel.codeUnits;
    final payloadSize = channelBytes.length + 1 + data.length;

    int bytesSent;
    if (payloadSize <= _lcmShortMessageMaxSize) {
      bytesSent = _publishShort(channel, channelBytes, data);
    } else {
      bytesSent = _publishFragmented(channel, channelBytes, data);
    }

    _stats.messagesSent++;
    _stats.bytesSent += bytesSent;

    _log('SEND channel="$channel" size=${data.length} bytes=$bytesSent seq=${_messageSequenceNumber - 1}');

    return bytesSent;
  }

  int _publishShort(String channel, List<int> channelBytes, Uint8List data) {
    // Create header
    final headerSize = 8; // magic (4) + seqno (4)
    final packetSize = headerSize + channelBytes.length + 1 + data.length;
    final packet = Uint8List(packetSize);
    final buffer = LcmBuffer.fromUint8List(packet);

    // Write header
    buffer.putUint32(_lcmMagicShort);
    buffer.putUint32(_messageSequenceNumber++);

    // Write channel name (null-terminated)
    buffer.putUint8List(channelBytes);
    buffer.putUint8(0);

    // Write data
    buffer.putUint8List(data);

    // Send packet
    final bytesSent = _sendSocket!.send(packet, _multicastAddress!, _port);
    return bytesSent;
  }

  int _publishFragmented(String channel, List<int> channelBytes, Uint8List data) {
    final fragmentSize = _lcmFragmentMaxPayload;
    final payloadSize = channelBytes.length + 1 + data.length;
    final numFragments = (payloadSize / fragmentSize).ceil();

    if (numFragments > 65535) {
      throw ArgumentError('Message too large to fragment');
    }

    final seqno = _messageSequenceNumber++;
    var fragmentOffset = 0;
    var dataOffset = 0;

    // Send first fragment (includes channel name)
    final firstFragmentDataSize = fragmentSize - (channelBytes.length + 1);
    _sendFragment(
      seqno,
      data.length,
      0,
      0,
      numFragments,
      channel,
      channelBytes,
      data.sublist(0, firstFragmentDataSize),
    );
    dataOffset += firstFragmentDataSize;
    fragmentOffset += firstFragmentDataSize;

    // Send remaining fragments
    for (var fragNo = 1; fragNo < numFragments; fragNo++) {
      final fragDataSize = (dataOffset + fragmentSize <= data.length)
          ? fragmentSize
          : data.length - dataOffset;

      _sendFragment(
        seqno,
        data.length,
        fragmentOffset,
        fragNo,
        numFragments,
        channel,
        null,
        data.sublist(dataOffset, dataOffset + fragDataSize),
      );

      dataOffset += fragDataSize;
      fragmentOffset += fragDataSize;
    }

    return data.length;
  }

  void _sendFragment(
    int seqno,
    int msgSize,
    int fragmentOffset,
    int fragmentNo,
    int fragmentsInMsg,
    String channel,
    List<int>? channelBytes,
    Uint8List fragmentData,
  ) {
    // Calculate packet size
    final headerSize = 20; // magic (4) + seqno (4) + msg_size (4) + frag_offset (4) + frag_no (2) + frags_in_msg (2)
    final channelSize = (channelBytes != null) ? channelBytes.length + 1 : 0;
    final packetSize = headerSize + channelSize + fragmentData.length;

    final packet = Uint8List(packetSize);
    final buffer = LcmBuffer.fromUint8List(packet);

    // Write header
    buffer.putUint32(_lcmMagicLong);
    buffer.putUint32(seqno);
    buffer.putUint32(msgSize);
    buffer.putUint32(fragmentOffset);
    buffer.putUint16(fragmentNo);
    buffer.putUint16(fragmentsInMsg);

    // Write channel name for first fragment
    if (channelBytes != null) {
      buffer.putUint8List(channelBytes);
      buffer.putUint8(0);
    }

    // Write fragment data
    buffer.putUint8List(fragmentData);

    // Send packet
    _sendSocket!.send(packet, _multicastAddress!, _port);
  }

  /// Subscribe to messages on the given channel.
  ///
  /// [channel] - Channel name or regex pattern to subscribe to
  /// [handler] - Callback function to handle received messages
  ///
  /// Returns an [LcmSubscription] object that can be used to unsubscribe.
  LcmSubscription subscribe(String channel, LcmMessageHandler handler) {
    if (_closed) {
      throw StateError('LCM instance has been closed');
    }

    // Check if channel contains regex metacharacters
    // If not, use fast exact string matching instead of regex
    final regexChars = RegExp(r'[.*+?^${}()|[\]\\]');
    final isExactMatch = !regexChars.hasMatch(channel);

    LcmSubscription subscription;
    if (isExactMatch) {
      // Fast path: exact string matching
      subscription = LcmSubscription._(channel, null, handler, true);
    } else {
      // Slow path: regex matching (LCM channels are implicitly anchored)
      final pattern = RegExp('^$channel\$');
      subscription = LcmSubscription._(channel, pattern, handler, false);
    }

    _subscriptions.add(subscription);
    _log('SUBSCRIBE channel="$channel" exact=$isExactMatch total=${_subscriptions.length}');
    return subscription;
  }

  /// Unsubscribe from a channel.
  ///
  /// [subscription] - The subscription object returned by [subscribe]
  void unsubscribe(LcmSubscription subscription) {
    _subscriptions.remove(subscription);
    _log('UNSUBSCRIBE channel="${subscription._channelPattern}" total=${_subscriptions.length}');
  }

  void _handleSocketEvent(RawSocketEvent event) {
    if (event == RawSocketEvent.read) {
      // Drain all available datagrams to reduce latency
      // This prevents multiple event loop iterations for back-to-back packets
      Datagram? datagram;
      while ((datagram = _recvSocket!.receive()) != null) {
        _handleDatagram(datagram!);
      }
    }
  }

  void _handleDatagram(Datagram datagram) {
    final receiveTimestamp = DateTime.now().microsecondsSinceEpoch;
    _stats._lastReceiveTimestamp = receiveTimestamp;

    final data = datagram.data;
    if (data.length < 8) {
      // Too short to be valid LCM packet
      _log('RECV invalid packet: too short (${data.length} bytes)');
      return;
    }

    _stats.bytesReceived += data.length;

    final buffer = LcmBuffer.fromUint8List(data);
    final magic = buffer.getUint32();

    if (magic == _lcmMagicShort) {
      _handleShortMessage(buffer, data, receiveTimestamp);
    } else if (magic == _lcmMagicLong) {
      _handleFragmentedMessage(buffer, data, datagram.address, receiveTimestamp);
    }
  }

  void _handleShortMessage(LcmBuffer buffer, Uint8List data, int receiveTimestamp) {
    // Read sequence number
    final seqno = buffer.getUint32();

    // Read channel name (null-terminated)
    final channelStart = buffer.position;
    var channelEnd = channelStart;
    while (channelEnd < data.length && data[channelEnd] != 0) {
      channelEnd++;
    }

    if (channelEnd >= data.length) {
      // Invalid packet: no null terminator
      _log('RECV invalid packet: no null terminator');
      return;
    }

    // Use view for channel name to avoid copy
    final channel = String.fromCharCodes(
      Uint8List.sublistView(data, channelStart, channelEnd)
    );
    final dataStart = channelEnd + 1; // Skip null terminator

    _stats.messagesReceived++;
    _log('RECV channel="$channel" size=${data.length - dataStart} seq=$seqno');

    // Use view for message data to avoid copy (receiver must copy if needed)
    final messageData = Uint8List.sublistView(data, dataStart);

    // Dispatch to subscribers
    _dispatchMessage(channel, messageData);
  }

  void _handleFragmentedMessage(LcmBuffer buffer, Uint8List data, InternetAddress from, int receiveTimestamp) {
    final seqno = buffer.getUint32();
    final msgSize = buffer.getUint32();
    final fragmentOffset = buffer.getUint32();
    final fragmentNo = buffer.getUint16();
    final fragmentsInMsg = buffer.getUint16();

    _stats.fragmentsReceived++;
    _log('RECV fragment seq=$seqno frag=$fragmentNo/$fragmentsInMsg offset=$fragmentOffset');

    // Create fragment buffer key
    final key = _makeFragmentKey(from, seqno);

    // Get or create fragment buffer
    var fragBuf = _fragmentBuffers[key];
    if (fragBuf == null || fragBuf.dataSize != msgSize) {
      // Clean up old fragment buffer if present
      if (fragBuf != null) {
        _stats.fragmentsDropped += fragBuf.fragmentsRemaining;
        _log('RECV fragment buffer reset: old seq=${fragBuf.seqno} had ${fragBuf.fragmentsRemaining} remaining');
      }
      _fragmentBuffers.remove(key);

      fragBuf = _FragmentBuffer(
        seqno: seqno,
        dataSize: msgSize,
        fragmentsRemaining: fragmentsInMsg,
      );
      _fragmentBuffers[key] = fragBuf;
    }

    // Read channel name if this is the first fragment
    if (fragmentNo == 0) {
      final channelStart = buffer.position;
      var channelEnd = channelStart;
      while (channelEnd < data.length && data[channelEnd] != 0) {
        channelEnd++;
      }

      if (channelEnd >= data.length) {
        // Invalid packet
        _fragmentBuffers.remove(key);
        _log('RECV invalid fragment: no channel terminator');
        return;
      }

      fragBuf.channel = String.fromCharCodes(data.sublist(channelStart, channelEnd));
      buffer.position = channelEnd + 1;
    }

    // Copy fragment data
    final fragmentData = data.sublist(buffer.position);
    final copyLength = fragmentData.length;

    if (fragmentOffset + copyLength > msgSize) {
      // Invalid fragment
      _fragmentBuffers.remove(key);
      _log('RECV invalid fragment: offset $fragmentOffset + $copyLength > $msgSize');
      return;
    }

    fragBuf.data!.setRange(fragmentOffset, fragmentOffset + copyLength, fragmentData);
    fragBuf.fragmentsRemaining--;

    // Check if message is complete
    if (fragBuf.fragmentsRemaining == 0) {
      _stats.messagesReceived++;
      _log('RECV complete channel="${fragBuf.channel}" size=$msgSize (reassembled from $fragmentsInMsg fragments)');
      _dispatchMessage(fragBuf.channel!, fragBuf.data!);
      _fragmentBuffers.remove(key);
    }
  }

  int _makeFragmentKey(InternetAddress from, int seqno) {
    // Simple key: combine address hash and sequence number
    return from.hashCode ^ seqno;
  }

  void _dispatchMessage(String channel, Uint8List data) {
    final dispatchStart = DateTime.now().microsecondsSinceEpoch;
    var matchCount = 0;

    for (final subscription in _subscriptions) {
      if (subscription.matches(channel)) {
        matchCount++;
        try {
          subscription._handler(channel, data);
        } catch (e, stackTrace) {
          _stats.dispatchErrors++;
          _log('DISPATCH ERROR channel="$channel": $e');
          stderr.writeln('Error in LCM message handler for channel $channel: $e');
          stderr.writeln(stackTrace);
        }
      }
    }

    final dispatchTime = DateTime.now().microsecondsSinceEpoch - dispatchStart;
    if (_debugEnabled && matchCount > 0) {
      _log('DISPATCH channel="$channel" handlers=$matchCount time=${dispatchTime}us');
    }
  }

  /// Close the LCM instance and release all resources.
  void close() {
    if (_closed) {
      return;
    }

    _log('CLOSE $_stats');

    _closed = true;
    _recvSubscription?.cancel();
    _recvSocket?.close();
    _sendSocket?.close();
    _subscriptions.clear();
    _fragmentBuffers.clear();
  }

  /// Print current statistics summary to debug log
  void logStats() {
    _log('STATS $_stats');
  }
}

/// Internal class to track fragmented messages being reassembled
class _FragmentBuffer {
  final int seqno;
  final int dataSize;
  String? channel;
  Uint8List? data;
  int fragmentsRemaining;

  _FragmentBuffer({
    required this.seqno,
    required this.dataSize,
    required this.fragmentsRemaining,
  }) {
    data = Uint8List(dataSize);
  }
}
