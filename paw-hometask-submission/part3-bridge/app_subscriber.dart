// ============================================================
// PAW COLLAR - FLUTTER MQTT SUBSCRIBER
// ============================================================
//
// Minimal working subscriber using the `mqtt_client` package
// (pub.dev: mqtt_client). Subscribes to every collar's location
// topic, decodes the JSON payload, and exposes the latest
// position per device_id so a map widget can update markers.
//
// pubspec.yaml:
//   dependencies:
//     mqtt_client: ^10.0.0
//
// ============================================================

import 'dart:convert';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

class CollarLocation {
  final int deviceId;
  final double lat;
  final double lon;
  final int ts;
  final int batteryMv;

  CollarLocation({
    required this.deviceId,
    required this.lat,
    required this.lon,
    required this.ts,
    required this.batteryMv,

  });

  factory CollarLocation.fromJson(int deviceId, Map<String, dynamic> json) {
    return CollarLocation(
      deviceId: deviceId,
      lat: (json['lat'] as num).toDouble(),
      lon: (json['lon'] as num).toDouble(),
      ts: json['ts'] as int,
      batteryMv: json['battery_mv'] as int,
    );
  }
}

/// Callback fired every time a fresh location arrives for any collar.
typedef OnLocationUpdate = void Function(CollarLocation location);

class PawMqttService {
  final String broker;
  final int port;
  final String clientId;
  final OnLocationUpdate onLocationUpdate;
  final void Function(bool gatewayOnline)? onGatewayStatus;

  late MqttServerClient _client;

  // Track the last sequence number seen per collar, so a duplicate
  // redelivery (QoS 1 can redeliver) doesn't trigger a redundant
  // marker animation on the map.
  final Map<int, int> _lastSeqByDevice = {};

  PawMqttService({
    required this.broker,
    required this.onLocationUpdate,
    this.onGatewayStatus,
    this.port = 1883,
    String? clientId,
  }) : clientId = clientId ?? 'paw_app_${DateTime.now().millisecondsSinceEpoch}';

  Future<void> connect() async {
    _client = MqttServerClient.withPort(broker, clientId, port);
    _client.logging(on: false);
    _client.keepAlivePeriod = 30;
    _client.autoReconnect = true;
    _client.resubscribeOnAutoReconnect = true;

    _client.onConnected = _onConnected;
    _client.onDisconnected = _onDisconnected;
    _client.onAutoReconnect = () => print('MQTT: auto-reconnecting...');

    final connMessage = MqttConnectMessage()
        .withClientIdentifier(clientId)
        .startClean(); // clean session: app only cares about "now"

    _client.connectionMessage = connMessage;

    try {
      await _client.connect();
    } catch (e) {
      print('MQTT connect failed: $e');
      _client.disconnect();
      return;
    }

    if (_client.connectionStatus?.state != MqttConnectionState.connected) {
      print('MQTT connection not established');
      return;
    }

    // Wildcard subscribe: one subscription covers every collar.
    // QoS 1 matches the publisher's QoS -- the broker won't
    // downgrade delivery below what we ask for here.
    _client.subscribe('paw/collar/+/location', MqttQos.atLeastOnce);
    _client.subscribe('paw/collar/gateway/status', MqttQos.atLeastOnce);

    _client.updates!.listen(_handleMessage);
  }

  void _onConnected() => print('MQTT connected');

  void _onDisconnected() => print('MQTT disconnected');

  void _handleMessage(List<MqttReceivedMessage<MqttMessage>> events) {
    for (final event in events) {
      final topic = event.topic;
      final recMess = event.payload as MqttPublishMessage;
      final payload =
          MqttPublishPayload.bytesToStringAsString(recMess.payload.message);

      if (topic == 'paw/collar/gateway/status') {
        onGatewayStatus?.call(payload == 'online');
        continue;
      }

      // topic shape: paw/collar/{device_id}/location
      final parts = topic.split('/');
      if (parts.length != 4) continue;

      final deviceId = int.tryParse(parts[2]);
      if (deviceId == null) continue;

      try {
        final json = jsonDecode(payload) as Map<String, dynamic>;
        final location = CollarLocation.fromJson(deviceId, json);

        // De-dupe QoS-1 redeliveries of the same fix.
        final lastSeq = _lastSeqByDevice[deviceId];
        if (lastSeq == location.seq) continue;
        _lastSeqByDevice[deviceId] = location.seq;

        onLocationUpdate(location);
      } catch (e) {
        print('Failed to decode payload on $topic: $e');
      }
    }
  }

  void disconnect() => _client.disconnect();
}


