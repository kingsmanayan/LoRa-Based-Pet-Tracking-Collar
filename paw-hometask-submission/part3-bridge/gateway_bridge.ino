// ============================================================
// PAW LORA GATEWAY BRIDGE
// LoRa (RAK3172 P2P) -> ESP32 -> MQTT
// ============================================================
//
// Fixes applied vs original draft:
//  1. Timer callbacks now match TimerCallbackFunction_t's real
//     signature (void(TimerHandle_t)) instead of being cast from
//     no-arg functions (was undefined behavior).
//  2. Reconnect calls are guarded against overlapping attempts.
//  3. Added a retained MQTT Last-Will/status topic so the app
//     can tell if the gateway itself goes offline.
//  4. decodeAndPublish() now checks mqttClient.connected()
//     before attempting a publish, and reports drops distinctly
//     from encode/parse errors.
//  5. Flagged the +EVT:RXP2P: field assumption -- confirm against
//     your RAK3172 firmware's actual serial log; some RUI3 builds
//     insert a length field before the hex payload.
//
// ============================================================

#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <ArduinoJson.h>

extern "C" {
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
}

// ============================================================
// WIFI CONFIGURATION
// ============================================================

const char* WIFI_SSID     = "YOUR_WIFI_NAME";
const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";

// ============================================================
// MQTT CONFIGURATION
// ============================================================

const char* MQTT_HOST = "broker.hivemq.com";
const uint16_t MQTT_PORT = 1883;

const char* MQTT_USERNAME = "";
const char* MQTT_PASSWORD = "";

const char* MQTT_BASE_TOPIC   = "paw/collar";
const char* MQTT_STATUS_TOPIC = "paw/collar/gateway/status";

// ============================================================
// ESP32 <-> RAK3172 UART2
// ============================================================

#define RAK_RX_PIN 16   // ESP32 RX2 <- RAK3172 TX
#define RAK_TX_PIN 17   // ESP32 TX2 -> RAK3172 RX

HardwareSerial RAKSerial(2);

// ============================================================
// LORA P2P CONFIGURATION
// ============================================================

#define LORA_FREQUENCY 868000000UL
#define LORA_SF        9
#define LORA_BW        125
#define LORA_CR        0
#define LORA_PREAMBLE  8
#define LORA_POWER     15

// ============================================================
// PACKET CONFIGURATION
// ============================================================

#define PACKET_SIZE      18
#define PROTOCOL_VERSION 1

// ============================================================
// MQTT / RECONNECT STATE
// ============================================================

AsyncMqttClient mqttClient;

TimerHandle_t mqttReconnectTimer;
TimerHandle_t wifiReconnectTimer;

volatile bool wifiConnectInFlight = false;
volatile bool mqttConnectInFlight = false;

String rakLine;

// ============================================================
// FORWARD DECLARATIONS
// ============================================================

void connectToWiFi();
void connectToMQTT();
void processRAKLine(String line);
void decodeAndPublish(String hexPayload, int rssi, int snr);

// ============================================================
// WIFI CONNECT
// ============================================================

void connectToWiFi()
{
    if (wifiConnectInFlight || WiFi.status() == WL_CONNECTED)
        return;

    wifiConnectInFlight = true;

    Serial.println();
    Serial.println("Connecting to Wi-Fi...");

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// ============================================================
// MQTT CONNECT
// ============================================================

void connectToMQTT()
{
    if (WiFi.status() != WL_CONNECTED)
        return;

    if (mqttConnectInFlight || mqttClient.connected())
        return;

    mqttConnectInFlight = true;

    Serial.println("Connecting to MQTT broker...");

    mqttClient.connect();
}

// ============================================================
// TIMER CALLBACK WRAPPERS
// (xTimerCreate requires void(TimerHandle_t) exactly)
// ============================================================

void onWifiReconnectTimer(TimerHandle_t xTimer)
{
    wifiConnectInFlight = false;
    connectToWiFi();
}

void onMqttReconnectTimer(TimerHandle_t xTimer)
{
    mqttConnectInFlight = false;
    connectToMQTT();
}

// ============================================================
// WIFI EVENT
// ============================================================

void WiFiEvent(WiFiEvent_t event)
{
    switch (event)
    {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:

            wifiConnectInFlight = false;

            Serial.println();
            Serial.println("Wi-Fi connected");
            Serial.print("IP address: ");
            Serial.println(WiFi.localIP());

            connectToMQTT();

            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:

            wifiConnectInFlight = false;

            Serial.println();
            Serial.println("Wi-Fi disconnected");

            xTimerStop(mqttReconnectTimer, 0);
            xTimerStart(wifiReconnectTimer, 0);

            break;

        default:
            break;
    }
}

// ============================================================
// MQTT CALLBACKS
// ============================================================

void onMqttConnect(bool sessionPresent)
{
    mqttConnectInFlight = false;

    Serial.println();
    Serial.println("MQTT connected");
    Serial.print("Session present: ");
    Serial.println(sessionPresent);

    // Announce we're online (retained, so a newly-opened app
    // sees gateway status immediately).
    mqttClient.publish(MQTT_STATUS_TOPIC, 1, true, "online");

    Serial.println("MQTT location publishing enabled");
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason)
{
    mqttConnectInFlight = false;

    Serial.println();
    Serial.println("MQTT disconnected");

    if (WiFi.status() == WL_CONNECTED)
    {
        xTimerStart(mqttReconnectTimer, 0);
    }
}

void onMqttPublish(uint16_t packetId)
{
    Serial.print("MQTT PUBACK received. Packet ID: ");
    Serial.println(packetId);
}

// ============================================================
// HEX HELPERS
// ============================================================

uint8_t hexCharToValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return 0xFF;
}

bool hexToBytes(const String& hex, uint8_t* buffer, size_t bufferSize)
{
    if (hex.length() % 2 != 0)
        return false;

    size_t byteCount = hex.length() / 2;

    if (byteCount != bufferSize)
        return false;

    for (size_t i = 0; i < byteCount; i++)
    {
        uint8_t high = hexCharToValue(hex[i * 2]);
        uint8_t low  = hexCharToValue(hex[i * 2 + 1]);

        if (high == 0xFF || low == 0xFF)
            return false;

        buffer[i] = (uint8_t)((high << 4) | low);
    }

    return true;
}

// ============================================================
// LITTLE-ENDIAN READERS
// ============================================================

int32_t readInt32LE(const uint8_t* p)
{
    uint32_t value =
        ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return (int32_t)value;
}

uint32_t readUInt32LE(const uint8_t* p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t readUInt16LE(const uint8_t* p)
{
    return ((uint16_t)p[0]) | ((uint16_t)p[1] << 8);
}

// ============================================================
// DECODE COLLAR PACKET -> MQTT
// ============================================================

void decodeAndPublish(String hexPayload, int rssi, int snr)
{
    hexPayload.trim();
    hexPayload.replace(" ", "");

    if (hexPayload.length() != PACKET_SIZE * 2)
    {
        Serial.println();
        Serial.println("ERROR: Invalid packet length");
        Serial.print("Expected HEX characters: ");
        Serial.println(PACKET_SIZE * 2);
        Serial.print("Received HEX characters: ");
        Serial.println(hexPayload.length());
        return;
    }

    uint8_t packet[PACKET_SIZE];

    if (!hexToBytes(hexPayload, packet, PACKET_SIZE))
    {
        Serial.println("ERROR: Invalid HEX payload");
        return;
    }

    uint8_t deviceId = packet[0];
    uint8_t protoVer = packet[1];
    uint8_t flags    = packet[2];

    int32_t  latRaw    = readInt32LE(&packet[3]);
    int32_t  lonRaw     = readInt32LE(&packet[7]);
    uint32_t timestamp  = readUInt32LE(&packet[11]);
    uint16_t batteryMv  = readUInt16LE(&packet[15]);
    uint8_t  sequence   = packet[17];

    if (protoVer != PROTOCOL_VERSION)
    {
        Serial.println();
        Serial.println("WARNING: Unsupported protocol version");
        Serial.print("Received: "); Serial.println(protoVer);
        Serial.print("Expected: "); Serial.println(PROTOCOL_VERSION);
        return;
    }

    double latitude  = latRaw / 1000000.0;
    double longitude = lonRaw / 1000000.0;

    bool gpsValid   = (flags & 0x01) != 0;
    bool motionWake = (flags & 0x02) != 0;

    Serial.println();
    Serial.println("==========================================");
    Serial.println("       LORA PACKET RECEIVED");
    Serial.println("==========================================");
    Serial.print("Device ID   : "); Serial.println(deviceId);
    Serial.print("Protocol    : "); Serial.println(protoVer);
    Serial.print("Latitude    : "); Serial.println(latitude, 6);
    Serial.print("Longitude   : "); Serial.println(longitude, 6);
    Serial.print("Timestamp   : "); Serial.println(timestamp);
    Serial.print("Battery     : "); Serial.print(batteryMv); Serial.println(" mV");
    Serial.print("Sequence    : "); Serial.println(sequence);
    Serial.print("GPS Valid   : "); Serial.println(gpsValid ? "YES" : "NO");
    Serial.print("Motion Wake : "); Serial.println(motionWake ? "YES" : "NO");
    Serial.print("RSSI        : "); Serial.println(rssi);
    Serial.print("SNR         : "); Serial.println(snr);

    if (!gpsValid)
    {
        // No fix this cycle -- nothing meaningful to show on the map.
        Serial.println("Skipping publish: GPS not valid for this packet");
        return;
    }

    if (!mqttClient.connected())
    {
        Serial.println("MQTT not connected -- dropping this update");
        // For a production build, buffer the last N packets in RAM
        // (or SPIFFS) and flush them once onMqttConnect() fires.
        return;
    }

    JsonDocument doc;

    doc["lat"]         = latitude;
    doc["lon"]         = longitude;
    doc["ts"]          = timestamp;
    doc["battery_mv"]  = batteryMv;
    doc["source"]      = "gateway";
    // doc["rssi"]        = rssi;
    // doc["snr"]         = snr;
    // doc["gps_valid"]   = gpsValid;
    // doc["motion_wake"] = motionWake;
    // doc["seq"]         = sequence;

    String mqttPayload;
    serializeJson(doc, mqttPayload);

    String topic = String(MQTT_BASE_TOPIC) + "/" + String(deviceId) + "/location";

    Serial.println();
    Serial.println("MQTT PUBLISH");
    Serial.print("Topic   : "); Serial.println(topic);
    Serial.print("Payload : "); Serial.println(mqttPayload);

    // QoS 1, retained -- see MQTT_SCHEMA.md for rationale.
    uint16_t packetId = mqttClient.publish(topic.c_str(), 1, true, mqttPayload.c_str());

    if (packetId != 0)
    {
        Serial.print("MQTT publish queued. Packet ID: ");
        Serial.println(packetId);
    }
    else
    {
        Serial.println("MQTT publish FAILED");
    }
}

// ============================================================
// PROCESS RAK3172 EVENT LINE
// ============================================================
//
// Expected RUI3 event:
//   +EVT:RXP2P:<RSSI>:<SNR>:<HEX_PAYLOAD>
//
// NOTE: some RUI3 firmware versions insert a length field:
//   +EVT:RXP2P:<RSSI>:<SNR>:<LEN>:<HEX_PAYLOAD>
// Check your module's actual serial output before field use --
// if a length field is present, split one extra segment here.
// ============================================================

void processRAKLine(String line)
{
    line.trim();

    if (line.length() == 0)
        return;

    Serial.print("RAK >> ");
    Serial.println(line);

    if (!line.startsWith("+EVT:RXP2P:"))
        return;

    String data = line.substring(String("+EVT:RXP2P:").length());

    int sep1 = data.indexOf(':');
    if (sep1 < 0) return;

    int rssi = data.substring(0, sep1).toInt();
    data = data.substring(sep1 + 1);

    int sep2 = data.indexOf(':');
    if (sep2 < 0) return;

    int snr = data.substring(0, sep2).toInt();
    String hexPayload = data.substring(sep2 + 1);

    decodeAndPublish(hexPayload, rssi, snr);
}

// ============================================================
// READ RAK3172 UART
// ============================================================

void readRAK3172()
{
    while (RAKSerial.available())
    {
        char c = RAKSerial.read();

        if (c == '\n')
        {
            if (rakLine.length() > 0)
            {
                processRAKLine(rakLine);
                rakLine = "";
            }
        }
        else if (c != '\r')
        {
            rakLine += c;

            if (rakLine.length() > 600)
                rakLine = "";
        }
    }
}

// ============================================================
// SEND AT COMMAND
// ============================================================

void sendRAKCommand(const String& command, uint32_t waitMs = 500)
{
    Serial.print("RAK COMMAND << ");
    Serial.println(command);

    RAKSerial.print(command);
    RAKSerial.print("\r\n");

    delay(waitMs);

    while (RAKSerial.available())
    {
        String response = RAKSerial.readStringUntil('\n');
        response.trim();

        if (response.length() > 0)
        {
            Serial.print("RAK RESPONSE >> ");
            Serial.println(response);
        }
    }
}

// ============================================================
// CONFIGURE RAK3172
// ============================================================

void configureRAK3172()
{
    Serial.println();
    Serial.println("==========================================");
    Serial.println("       CONFIGURING RAK3172");
    Serial.println("==========================================");

    sendRAKCommand("AT", 500);
    sendRAKCommand("ATE0", 500);
    sendRAKCommand("AT+NWM=0", 500);   // P2P mode

    String p2pCommand =
        "AT+P2P=" + String(LORA_FREQUENCY) + ":" + String(LORA_SF) + ":" +
        String(LORA_BW) + ":" + String(LORA_CR) + ":" + String(LORA_PREAMBLE) +
        ":" + String(LORA_POWER);

    sendRAKCommand(p2pCommand, 1000);
    sendRAKCommand("AT+PRECV=65534", 1000);   // continuous receive

    Serial.println();
    Serial.println("RAK3172 READY -- LoRa P2P, 868 MHz, SF9, CR 4/5, continuous RX");
}

// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("==========================================");
    Serial.println("        PAW LORA GATEWAY");
    Serial.println("==========================================");

    RAKSerial.begin(115200, SERIAL_8N1, RAK_RX_PIN, RAK_TX_PIN);
    delay(1000);

    mqttReconnectTimer = xTimerCreate(
        "mqttTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, onMqttReconnectTimer);

    wifiReconnectTimer = xTimerCreate(
        "wifiTimer", pdMS_TO_TICKS(2000), pdFALSE, (void*)0, onWifiReconnectTimer);

    WiFi.onEvent(WiFiEvent);
    WiFi.mode(WIFI_STA);

    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onPublish(onMqttPublish);
    mqttClient.setServer(MQTT_HOST, MQTT_PORT);

    // Retained LWT: if the gateway drops off ungracefully, subscribers
    // find out immediately instead of just going quiet.
    mqttClient.setWill(MQTT_STATUS_TOPIC, 1, true, "offline");

    if (strlen(MQTT_USERNAME) > 0)
    {
        mqttClient.setCredentials(MQTT_USERNAME, MQTT_PASSWORD);
    }

    connectToWiFi();
    configureRAK3172();

    Serial.println();
    Serial.println("        GATEWAY SYSTEM READY");
}

// ============================================================
// LOOP
// ============================================================

void loop()
{
    readRAK3172();
    delay(2);
}
