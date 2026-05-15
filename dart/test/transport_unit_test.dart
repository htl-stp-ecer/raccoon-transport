import 'dart:async';
import 'dart:typed_data';

import 'package:raccoon_transport/messages/protocol/ack_t.g.dart';
import 'package:raccoon_transport/messages/protocol/envelope_t.g.dart';
import 'package:raccoon_transport/messages/protocol/retain_request_t.g.dart';
import 'package:raccoon_transport/src/channels.dart';
import 'package:raccoon_transport/src/lcm/lcm.dart';
import 'package:raccoon_transport/src/lcm/lcm_buffer.dart';
import 'package:raccoon_transport/src/reliable.dart';
import 'package:raccoon_transport/src/retain.dart';
import 'package:raccoon_transport/src/transport.dart';
import 'package:test/test.dart';

int _portSeed = 18000;
int _channelSeed = 0;

String uniqueProvider() => 'udpm://239.255.76.67:${_portSeed++}?ttl=0';
String uniqueChannel(String prefix) => '$prefix/${_channelSeed++}';

Future<void> pump([Duration duration = const Duration(milliseconds: 20)]) async {
  await Future<void>.delayed(duration);
}

Future<void> eventually(void Function() assertion,
    {Duration timeout = const Duration(seconds: 2),
    Duration step = const Duration(milliseconds: 20)}) async {
  final deadline = DateTime.now().add(timeout);
  Object? lastError;
  StackTrace? lastStack;

  while (DateTime.now().isBefore(deadline)) {
    try {
      assertion();
      return;
    } catch (error, stack) {
      lastError = error;
      lastStack = stack;
      await Future<void>.delayed(step);
    }
  }

  if (lastError != null) {
    Error.throwWithStackTrace(lastError!, lastStack!);
  }
  throw StateError('eventually() timed out without assertion details');
}

Uint8List encodeManualRetainRequest(String channel) {
  final channelBytes = channel.codeUnits;
  final buf = LcmBuffer(8 + 8 + 4 + channelBytes.length + 4);
  buf.putInt64(0);
  buf.putInt64(0);
  buf.putInt32(channelBytes.length);
  buf.putUint8List(channelBytes);
  buf.putInt32(0);
  return Uint8List.sublistView(buf.uint8List, 0, buf.position);
}

void main() {
  test('requestRetained replays cached payload', () async {
    final provider = uniqueProvider();
    final publisher = await RaccoonTransport.create(provider);
    final subscriber = await RaccoonTransport.create(provider);
    final channel = uniqueChannel('unit/retain');
    final cached = Uint8List.fromList([9, 8, 7, 6]);
    final received = <Uint8List>[];

    try {
      publisher.publish(channel, cached, options: const PublishOptions(retained: true));
      await pump(const Duration(milliseconds: 50));
      subscriber.subscribe(channel, (_, data) => received.add(Uint8List.fromList(data)),
          options: const SubscribeOptions(requestRetained: true));

      await eventually(() {
        expect(received, hasLength(1));
        expect(received.single, orderedEquals(cached));
      });
    } finally {
      subscriber.dispose();
      publisher.dispose();
    }
  });

  test('requestRetained missing channel is silent', () async {
    final transport = await RaccoonTransport.create(uniqueProvider());
    final channel = uniqueChannel('unit/missing');
    var called = false;

    try {
      transport.subscribe(channel, (_, __) => called = true,
          options: const SubscribeOptions(requestRetained: true));
      await pump(const Duration(milliseconds: 100));
      expect(called, isFalse);
    } finally {
      transport.dispose();
    }
  });

  test('retain store accepts generated request format', () async {
    final lcm = await Lcm.create(uniqueProvider());
    final retainStore = RetainStore();
    final channel = uniqueChannel('unit/generated-request');
    final payload = Uint8List.fromList([4, 2, 4, 2]);
    final received = <Uint8List>[];

    try {
      retainStore.startListening(lcm);
      retainStore.cache(channel, payload);
      lcm.subscribe(channel, (_, data) => received.add(Uint8List.fromList(data)));

      final request = RetainRequestT(
        timestamp: 111,
        channel: channel,
        subscriber_id: 'subscriber',
      );
      final buf = LcmBuffer(256);
      request.encode(buf);
      lcm.publish(
        ProtocolChannels.retainRequest,
        Uint8List.sublistView(buf.uint8List, 0, buf.position),
      );

      await eventually(() {
        expect(received, hasLength(1));
        expect(received.single, orderedEquals(payload));
      });
    } finally {
      lcm.close();
    }
  });

  test('retain store accepts manual interop request format', () async {
    final lcm = await Lcm.create(uniqueProvider());
    final retainStore = RetainStore();
    final channel = uniqueChannel('unit/manual-request');
    final payload = Uint8List.fromList([8, 6, 7, 5, 3, 0, 9]);
    final received = <Uint8List>[];

    try {
      retainStore.startListening(lcm);
      retainStore.cache(channel, payload);
      lcm.subscribe(channel, (_, data) => received.add(Uint8List.fromList(data)));
      lcm.publish(ProtocolChannels.retainRequest, encodeManualRetainRequest(channel));

      await eventually(() {
        expect(received, hasLength(1));
        expect(received.single, orderedEquals(payload));
      });
    } finally {
      lcm.close();
    }
  });

  test('reliable subscriber deduplicates but ACKs every delivery', () async {
    final lcm = await Lcm.create(uniqueProvider());
    final subscriber = ReliableSubscriber(lcm, 'subscriber-1');
    final channel = uniqueChannel('unit/reliable');
    final payload = Uint8List.fromList([7, 7, 1]);
    final deliveries = <({String channel, Uint8List data})>[];
    final acks = <AckT>[];

    try {
      subscriber.subscribe(channel,
          (receivedChannel, data) => deliveries.add((channel: receivedChannel, data: Uint8List.fromList(data))));
      lcm.subscribe(ProtocolChannels.ack, (_, data) {
        acks.add(AckT.decode(LcmBuffer.fromUint8List(data)));
      });

      final env = EnvelopeT(
        timestamp: 123456789,
        publisher_id: 'publisher-1',
        seq_num: 42,
        channel: channel,
        payload_size: payload.length,
        payload: payload,
      );
      final buf = LcmBuffer(256);
      env.encode(buf);
      final encoded = Uint8List.sublistView(buf.uint8List, 0, buf.position);

      final reliableChannel = ProtocolChannels.reliableChannel(channel);
      lcm.publish(reliableChannel, encoded);
      lcm.publish(reliableChannel, encoded);

      await eventually(() {
        expect(deliveries, hasLength(1));
        expect(deliveries.single.channel, channel);
        expect(deliveries.single.data, orderedEquals(payload));
        expect(acks, hasLength(2));
      });

      for (final ack in acks) {
        expect(ack.publisher_id, 'publisher-1');
        expect(ack.seq_num, 42);
        expect(ack.subscriber_id, 'subscriber-1');
      }
    } finally {
      subscriber.dispose();
      lcm.close();
    }
  });

  test('reliable publisher stops retrying after matching ack', () async {
    final lcm = await Lcm.create(uniqueProvider());
    final publisher = ReliablePublisher(lcm, 'publisher-2');
    final channel = uniqueChannel('unit/ack-stop');
    final payload = Uint8List.fromList([1, 9, 9]);
    final envelopes = <EnvelopeT>[];

    try {
      publisher.startListening();
      lcm.subscribe(ProtocolChannels.reliableChannel(channel), (_, data) {
        envelopes.add(EnvelopeT.decode(LcmBuffer.fromUint8List(data)));
      });

      publisher.publish(channel, payload,
          retryInterval: Duration.zero, maxRetries: 5);

      await eventually(() => expect(envelopes, hasLength(1)));

      final ack = AckT(
        timestamp: 987654321,
        publisher_id: 'publisher-2',
        seq_num: 0,
        subscriber_id: 'subscriber-2',
      );
      final buf = LcmBuffer(256);
      ack.encode(buf);
      lcm.publish(
        ProtocolChannels.ack,
        Uint8List.sublistView(buf.uint8List, 0, buf.position),
      );

      await pump(const Duration(milliseconds: 150));
      expect(envelopes, hasLength(1));
    } finally {
      publisher.dispose();
      lcm.close();
    }
  });

  test('reliable publisher retries exactly until maxRetries', () async {
    final lcm = await Lcm.create(uniqueProvider());
    final publisher = ReliablePublisher(lcm, 'publisher-3');
    final channel = uniqueChannel('unit/retry-budget');
    final payload = Uint8List.fromList([5, 4, 3, 2, 1]);
    final envelopes = <EnvelopeT>[];

    try {
      lcm.subscribe(ProtocolChannels.reliableChannel(channel), (_, data) {
        envelopes.add(EnvelopeT.decode(LcmBuffer.fromUint8List(data)));
      });

      publisher.publish(channel, payload,
          retryInterval: Duration.zero, maxRetries: 3);

      await eventually(() => expect(envelopes, hasLength(3)),
          timeout: const Duration(seconds: 3));
      await pump(const Duration(milliseconds: 150));
      expect(envelopes, hasLength(3));

      for (final env in envelopes) {
        expect(env.publisher_id, 'publisher-3');
        expect(env.seq_num, 0);
        expect(env.channel, channel);
        expect(env.payload, orderedEquals(payload));
      }
    } finally {
      publisher.dispose();
      lcm.close();
    }
  });
}
