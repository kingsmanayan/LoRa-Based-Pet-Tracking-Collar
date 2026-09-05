# PAW — LoRa-Based Pet Tracking Collar

A battery-powered pet tracking collar that reports location over a private
868 MHz LoRa P2P link — no SIM, no cellular modem. A gateway board bridges
received LoRa packets onto MQTT so a mobile app can show live locations on a
map.

This repo contains all three parts of the assignment: the collar + gateway
hardware, the collar firmware, and the LoRa→MQTT bridge with an app-side
subscriber. `PAW_Full_Project_Documentation.docx` is the single consolidated
write-up; this README is the map of the repo and the "how do I run this"
guide.

## Repository structure

```
paw-hometask/
├── PAW_Full_Project_Documentation.pdf   ← consolidated project doc (start here)
├── README.md                             ← this file
├── part1-hardware/
│   ├── collar/
│   │   ├── LoRa_Based_Pet_Collar
│   │   │   ├──LoRa_Based_Pet_Collar.Kicad Schematric       ← collar main board KaiCad schematic
│   │   ├── collar_schematic.pdf / .jpg   ← collar main board schematic
│   │   ├── BOM_Pet_Tracking_Collar.pdf  ← collar bill of materials (₹3,696/unit)
│   │   └── Collar_Design_Notes.pdf      ← antenna placement, power budget, 1,000-unit changes
│   └── bridge/
│   │   ├── LoRa_Reciver_End_Bridge_part
│   │   │   ├──LORA_Bridge_part.Kicad Schematric       ← gateway (ESP32 + RAK3172) KaiCad schematic
│       ├── bridge_schematic.pdf / .jpg   ← gateway (ESP32 + RAK3172) schematic
│       ├── BOM_LoRa_Bridge_Receiver.pdf ← gateway bill of materials (₹2,102/unit)
│       └── Bridge_Design_Notes.pdf      ← antenna placement, power budget, 1,000-unit changes
├── part2-firmware/
│   ├── collar_firmware.cpp               ← collar firmware (RUI3 APIs, RAK3172)
│   ├── PAW_Technical_Document.pdf       ← architecture, state machine, deliverables summary
│   ├── PAW_Binary_Packet_Specification.pdf ← 18-byte packet layout + encode/decode reference
│   ├── PAW_LoRa_Radio_Configuration.pdf ← SF/CR/BW/power rationale, airtime, ETSI duty-cycle check
│   └── PAW_Power_Optimization_Strategy.pdf ← 7-point power design + battery-life scenarios
└── part3-bridge/
    ├── gateway_bridge.ino                ← ESP32 firmware: RAK3172 UART → decode → MQTT publish
    ├── MQTT_SCHEMA.md                    ← topic structure, payload schema, QoS rationale
    └── app_subscriber.dart               ← Flutter MQTT subscriber (mqtt_client package)
```

## What's real vs. simulated/stubbed

| Component | Status |
|---|---|
| Collar schematic (KiCad-style, hand-drawn to PDF) | Real, complete circuit |
| Collar firmware — accelerometer wake, sleep, packet build, LoRa TX | Real logic, targets real RUI3 APIs |
| Collar firmware — GPS acquisition | **Mockable.** `HW_GPS_MOCK` build switch (`collar_firmware.cpp`, top of file). `1` = simulated jittered fix (bench-test default); `0` = real NMEA `$GxGGA` parsing over UART, already implemented and ready for hardware |
| Collar firmware — LoRa TX | **Mockable.** `HW_LORA_MOCK` build switch. `1` = logs the packet instead of radioing it (bench test without a radio); `0` (default) = real `api.lora.psend()` |
| Gateway schematic | Real, complete circuit |
| Gateway bridge firmware | Real logic (WiFi/MQTT reconnect state machine, AT-command RAK3172 config, hex packet decode). **Not bench-tested against a live RAK3172 serial stream** — see Open Items below for the one field-format assumption that needs confirming against real hardware |
| MQTT broker | Not run/hosted as part of this submission — `gateway_bridge.ino` targets a public test broker (`broker.hivemq.com`) by default; point `MQTT_HOST` at your own broker (Mosquitto, HiveMQ Cloud, etc.) for anything beyond a bench test |
| App-side subscriber | Real, working Dart/Flutter code against the `mqtt_client` package — not wired into a full map UI, since the task asks for the subscription layer, not a complete app |
| Battery-life figures | **Engineering estimates**, not measurements — every current-draw number is a datasheet-typical value, clearly labeled as such in `PAW_Power_Optimization_Strategy.docx`. No physical hardware was available to measure real current draw or real per-animal duty cycle |

## How to run each part

### Part 1 — Hardware
Open `part1-hardware/collar/collar_schematic.pdf` and `part1-hardware/bridge/bridge_schematic.pdf`
directly (or the `.jpg` renders for a quick look without a PDF viewer). BOM and design-notes
documents are standard `.docx`, viewable in Word / LibreOffice / Google Docs.

### Part 2 — Firmware (collar)
Target: RAKwireless RAK3172 (STM32WLE5), built through the Arduino IDE with the RAKwireless
RUI3 board package installed (`RAK3172` board target).

1. Install the RUI3 Arduino board package (RAKwireless documentation) and select the RAK3172 board.
2. Open `part2-firmware/collar_firmware.cpp` in a sketch folder of the same name (Arduino requires
   the `.ino`/`.cpp` filename to match its containing folder — rename the folder or the file to suit).
3. Leave `HW_GPS_MOCK 1` and `HW_LORA_MOCK 0` to bench-test the full wake→fix→pack→transmit→sleep
   cycle over a real LoRa radio with a simulated GPS fix. Flip `HW_GPS_MOCK` to `0` once a MAX-M10S
   is wired to `Serial1`.
4. Flash and open the serial monitor at 115200 baud to watch the wake/fix/TX log lines.

### Part 3 — MQTT bridge
Target: any ESP32 dev board (schematic assumes an ESP32-DEVKIT-V1) wired to a RAK3172 over UART2
(pins 16/17), with the RAK3172 configured for continuous LoRa P2P receive.

1. Arduino IDE with ESP32 board support, plus the `AsyncMqttClient` and `ArduinoJson` libraries.
2. In `part3-bridge/gateway_bridge.ino`, set `WIFI_SSID` / `WIFI_PASSWORD` and, if not using the
   default public test broker, `MQTT_HOST` / `MQTT_USERNAME` / `MQTT_PASSWORD`.
3. Flash to the ESP32. On boot it configures the RAK3172 over AT commands (`AT+P2P=...`,
   `AT+PRECV=65534` for continuous RX) and begins forwarding decoded packets to
   `paw/collar/{device_id}/location` — see `MQTT_SCHEMA.md` for the full payload shape and the
   QoS-1 / retained rationale.
4. To bench-test without real LoRa hardware, inject a mock `+EVT:RXP2P:<rssi>:<snr>:<hex>` line on
   the UART the ESP32 reads from `RAKSerial` (e.g. from a second microcontroller or a USB-serial
   adapter looped into pin 16), using a hex payload produced by `PAW_Binary_Packet_Specification.docx`'s
   worked example.
5. App side: add `mqtt_client: ^10.0.0` to a Flutter project's `pubspec.yaml`, drop in
   `part3-bridge/app_subscriber.dart`, and call `PawMqttService(broker: '<your broker>',
   onLocationUpdate: ...).connect()`.

## Assumptions

- No physical hardware (RAK3172, MAX-M10S, LIS2DE12, ESP32) was available during this work —
  all firmware is written against real RUI3/Arduino APIs and is structured to run unmodified on
  the target hardware, but has only been exercised via the mock switches described above.
- LoRa P2P (not LoRaWAN) is used throughout, matching the assignment's "talks directly to the
  owner's phone... and to a home LoRaWAN gateway" framing loosely — the gateway in this submission
  receives LoRa P2P packets directly rather than acting as a full LoRaWAN network-server gateway,
  which is simpler to implement end-to-end within the timebox and still satisfies "gets a location
  fix into a mobile app over MQTT."
- Component prices in both BOMs are indicative single-unit distributor prices in INR as of
  September 2026 and exclude GST, assembly labour, and enclosure hardware (noted in each BOM).
- MQTT broker is assumed pre-existing infrastructure (a public test broker is wired in by default)
  rather than something this submission stands up itself.

## Open items / known inconsistencies

Working across the hardware, firmware, and bridge documents surfaced a few real inconsistencies
worth flagging rather than quietly resolving one way or the other — see
**Section 7 (Cross-Document Consistency Review)** of `PAW_Full_Project_Documentation.docx` for the
full list with reasoning. The two most consequential:

1. **The battery-life headline figures assume a regulator that isn't in the current BOM.**
   `PAW_Power_Optimization_Strategy.docx` models ~3 µA sleep current (a low-IQ regulator), but the
   collar schematic/BOM currently specifies an **AMS1117 LDO**, whose own datasheet-typical ~5 mA
   quiescent draw is called out in `Collar_Design_Notes.docx` as "the single biggest lever on
   standby battery life." At 5 mA continuous, a 1,000 mAh cell would deplete in roughly **8 days**
   regardless of GPS/LoRa activity — nowhere near the 6–8 month figures quoted elsewhere. The
   low-IQ regulator swap is listed as a production TODO in the design notes; until it's made, the
   long-runtime figures describe the production revision, not the schematic as currently drawn.
2. **LoRa bandwidth is never set explicitly in the collar firmware.** `hal_lora_init()` has no
   bandwidth call, so it relies on an unstated RUI3 P2P default (assumed 125 kHz). The gateway
   firmware does set it explicitly (`LORA_BW=125`), which is indirect evidence the link works at
   125 kHz — but the collar side should set it explicitly too, since an unstated default is one
   library update away from silently breaking the link.

## What I'd do differently with more time

- Get real hardware on the bench to replace every datasheet-typical current figure with a measured
  one, and to confirm the actual `+EVT:RXP2P` field format the target RUI3 firmware version emits
  (flagged directly in `gateway_bridge.ino`'s comments).
- Resolve the regulator/battery-life mismatch above by re-running the Section 6/7 battery scenarios
  against the AMS1117's real quiescent current, or by moving the low-IQ regulator into the current
  BOM instead of deferring it to the 1,000-unit revision.
- Wire up the payload fields `MQTT_SCHEMA.md` documents but `gateway_bridge.ino` currently leaves
  commented out (`rssi`, `snr`, `gps_valid`, `motion_wake`, `seq`) — and fix `app_subscriber.dart`'s
  `CollarLocation`, which references `location.seq` for de-dup but never declares or parses a `seq`
  field, so that logic doesn't currently compile/run end-to-end.
- Give the collar an RTC or have the gateway stamp `ts` on receipt, so the map can show wall-clock
  time instead of seconds-since-boot.
- Add battery discharge protection to the collar (TP4056 only manages charging) and a defined
  input/regulation stage to the gateway board (its schematic currently assumes a bare 3.3 V rail
  with no shown supply path) before either design is production-ready.
