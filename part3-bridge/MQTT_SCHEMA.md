# PAW Collar — MQTT Topic & Payload Schema

## 1. Topic structure

| Topic | Purpose | Retained? |
|---|---|---|
| `paw/collar/{device_id}/location` | Latest location update from one collar | Yes |
| `paw/collar/gateway/status` | Gateway online/offline (LWT) | Yes |

- `{device_id}` is the single-byte `DEVICE_ID` from the tracker firmware, published as a decimal string (e.g. `paw/collar/1/location`).
- The app subscribes to `paw/collar/+/location` to track every collar on one connection, or to a specific `paw/collar/1/location` if it only cares about one pet.
- `paw/collar/gateway/status` payload is the plain string `online` or `offline` (set via MQTT Last Will and Testament, so a crashed or disconnected gateway is reported automatically by the broker).

## 2. Location payload

```json
{
  "lat": 52.4104,
  "lon": 12.9738,
  "ts": 1735689600,
  "battery_mv": 3850,
  "source": "gateway",
 
}
```

| Field | Type | Description |
|---|---|---|
| `lat` | float | Latitude, decimal degrees |
| `lon` | float | Longitude, decimal degrees |
| `ts` | uint32 | Seconds since collar boot (see note below — not wall-clock UTC in the current tracker firmware) |
| `battery_mv` | uint16 | Collar battery voltage, millivolts |
| `source` | string | Always `"gateway"` — reserved for future multi-gateway dedup | 
| `rssi` | int | Signal strength of the received LoRa packet, dBm |
| `snr` | int | Signal-to-noise ratio of the received LoRa packet, dB |
| `gps_valid` | bool | Whether the collar had a real fix when it sent this packet |
| `motion_wake` | bool | `true` if this update was triggered by the accelerometer, `false` if it's the periodic 30-minute update |
| `seq` | uint8 | Rolling per-collar sequence number (0–255, wraps), useful for detecting dropped packets |

**Note on `ts`:** the current tracker firmware sets `gps_timestamp = millis() / 1000`, i.e. seconds since the collar last rebooted — not a real UTC epoch. If you need wall-clock time on the map, either read the NMEA `$GxGGA` time field into the packet, add an RTC to the tracker, or have the gateway stamp `ts` itself on receipt (accepting LoRa transit delay, which is sub-second and usually fine for a pet tracker).

## 3. QoS choice: **QoS 1, retained**

| QoS | Guarantee | Why (not) here |
|---|---|---|
| 0 | At-most-once, fire-and-forget | Rejected — LoRa fixes are expensive to reacquire (GPS cold-start can take up to a minute, and each transmission costs battery). Silently dropping a location update on a flaky Wi-Fi hop between gateway and broker isn't worth the marginal latency savings. |
| **1** | **At-least-once** | **Chosen.** Guarantees the update reaches the broker even across a brief gateway/broker disconnect (the broker re-delivers until PUBACK). Possible duplicate deliveries are harmless here — each message fully overwrites the previous location, so re-processing the same fix twice on the app side is a no-op. |
| 2 | Exactly-once | Rejected — the extra handshake (PUBREC/PUBREL/PUBCOMP) costs more round-trips and broker-side state for a benefit (dedup) the app doesn't need, since duplicate location messages are idempotent by nature. |

**Retain = true rationale:** the tracker only transmits every 30 minutes (or on motion). Without retain, a mobile app that opens or reconnects has to wait up to 30 minutes to see anything. With retain, the broker immediately replays the last known location for that topic the instant the app subscribes — the map is populated on open, not on the next transmission.

## 4. Suggested extension (multi-gateway dedup)

If you add a second gateway later, use `seq` + `device_id` (and optionally the tracker's own `ts`) to de-duplicate the same over-the-air packet received by two gateways at once, since `source` alone won't disambiguate.
