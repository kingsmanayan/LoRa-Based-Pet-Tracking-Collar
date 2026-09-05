/*
 * =====================================================================
 *  PAW PET TRACKER — Collar Firmware
 * =====================================================================
 *  Target      : RAKwireless RAK3172 (STM32WLE5) via RUI3 APIs
 *  Radio       : LoRa P2P, 868 MHz (EU868)
 *  GPS         : u-blox MAX-M10S over UART (NMEA GGA), mockable
 *  Accelerometer: ST LIS2DW12 over I2C, wake-on-motion interrupt
 *
 *  HARDWARE ABSTRACTION
 *  ---------------------------------------------------------------
 *  This file targets real RUI3 hardware but is structured so the
 *  hardware-facing calls are isolated behind small functions
 *  (hal_gps_*, hal_accel_*, hal_battery_*, hal_lora_*) so it can be
 *  ported to bare STM32 HAL, or run against the built-in GPS/LoRa
 *  simulators for bench testing without any radio/GPS hardware
 *  attached. Flip HW_GPS_MOCK / HW_LORA_MOCK below — no need to
 *  hunt through the logic to find where mocking happens.
 * =====================================================================
 */

#include <Arduino.h>
#include <Wire.h>

// =====================================================================
// BUILD-TIME HARDWARE SWITCHES  (flip these, nothing else)
// =====================================================================
#define HW_GPS_MOCK              1   // 1 = simulated NMEA fix, 0 = real MAX-M10S over UART
#define HW_LORA_MOCK             0   // 1 = log packets instead of radio TX (bench test w/o radio)

// =====================================================================
// CONFIGURATION
// =====================================================================
#define DEVICE_ID                0x01
#define PROTOCOL_VERSION         0x01

#define MAX_FIX_INTERVAL_MS      1800000UL     // 30 min: max silence even w/o motion
#define GPS_FIX_TIMEOUT_MS       60000UL       // give up on a fix after 60 s
#define MOTION_COOLDOWN_MS       120000UL      // min gap between motion-triggered TXs (2 min)

// LoRa P2P (EU868)
#define LORA_FREQUENCY            868000000UL
#define LORA_SPREADING_FACTOR     9
#define LORA_CODING_RATE          0             // 4/5
#define LORA_PREAMBLE             8
#define LORA_TX_POWER             14            // dBm

// GPS UART
#define GPS_BAUD                  9600
#define GPS_SERIAL                Serial1        // RAK3172 UART1 -> MAX-M10S

// LIS2DW12 (accelerometer)
#define LIS2DW12_ADDRESS          0x18
#define ACC_INT_PIN               WB_IO2

#define LIS2DW12_CTRL1            0x20
#define LIS2DW12_CTRL2            0x21
#define LIS2DW12_CTRL3            0x22
#define LIS2DW12_CTRL4_INT1       0x23
#define LIS2DW12_WAKE_UP_THS      0x34
#define LIS2DW12_WAKE_UP_DUR      0x35
#define LIS2DW12_WAKE_UP_SRC      0x38
#define LIS2DW12_CTRL7            0x3F

// =====================================================================
// PACKET LAYOUT  (18 bytes, all little-endian)
//   [0]      device_id
//   [1]      protocol_version
//   [2]      flags        bit0=gps_valid  bit1=motion_wake
//   [3..6]   latitude     int32,  degrees * 1e6
//   [7..10]  longitude    int32,  degrees * 1e6
//   [11..14] timestamp    uint32, seconds since boot
//   [15..16] battery_mv   uint16
//   [17]     seq          uint8, wraps at 256
// =====================================================================
#define PACKET_LEN 18

// =====================================================================
// GLOBAL STATE
// =====================================================================
volatile bool     g_motion_flag   = false;   // set in ISR, cleared once serviced
uint32_t          g_last_motion_tx_ms = 0;    // for motion cooldown
uint8_t           g_sequence      = 0;        // matches packed 1-byte field

bool     g_gps_valid     = false;
float    g_gps_lat       = 0.0f;
float    g_gps_lon       = 0.0f;
uint32_t g_gps_timestamp = 0;
uint16_t g_battery_mv    = 0;

// =====================================================================
// HAL: LIS2DW12 (I2C accelerometer)
// =====================================================================
static void lis2dw12_write(uint8_t reg, uint8_t value)
{
    Wire.beginTransmission(LIS2DW12_ADDRESS);
    Wire.write(reg);
    Wire.write(value);
    Wire.endTransmission();
}

static uint8_t lis2dw12_read(uint8_t reg)
{
    Wire.beginTransmission(LIS2DW12_ADDRESS);
    Wire.write(reg);
    Wire.endTransmission(false);
    Wire.requestFrom(LIS2DW12_ADDRESS, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

// Forward decl for the ISR
void accelerometerISR();

static void hal_accel_init()
{
    lis2dw12_write(LIS2DW12_CTRL1, 0x10);       // 12.5 Hz, low-power mode 1
    lis2dw12_write(LIS2DW12_CTRL2, 0x00);
    lis2dw12_write(LIS2DW12_CTRL3, 0x40);       // latched interrupt
    lis2dw12_write(LIS2DW12_CTRL4_INT1, 0x20);  // wake-up event -> INT1
    lis2dw12_write(LIS2DW12_WAKE_UP_THS, 4);    // motion threshold
    lis2dw12_write(LIS2DW12_WAKE_UP_DUR, 0x00);
    lis2dw12_write(LIS2DW12_CTRL7, 0x20);       // interrupts enabled

    pinMode(ACC_INT_PIN, INPUT);
    attachInterrupt(ACC_INT_PIN, accelerometerISR, RISING);

    Serial.println("[HAL] LIS2DW12 initialized");
}

static inline void hal_accel_clear_int()
{
    lis2dw12_read(LIS2DW12_WAKE_UP_SRC);
}

// =====================================================================
// ACCELEROMETER ISR
// ---------------------------------------------------------------------
// Kept minimal on purpose: an ISR should only set a flag. All real
// work (I2C reads, GPS acquisition, LoRa TX) happens later in
// motionHandler(), running in normal (non-interrupt) context via the
// RUI3 timer/event dispatch, not directly inside the ISR.
// =====================================================================
void accelerometerISR()
{
    g_motion_flag = true;
    // Defer the actual handling to a one-shot software timer so all
    // I2C/UART/radio work happens outside interrupt context.
    api.system.timer.start(RAK_TIMER_1, 10, NULL);
}

// =====================================================================
// HAL: GPS
// =====================================================================
static double nmeaToDecimal(const char *value, char direction)
{
    if (value == NULL || strlen(value) < 3) return 0.0;
    double raw = atof(value);
    int degrees = (int)(raw / 100);
    double minutes = raw - (degrees * 100);
    double decimal = degrees + (minutes / 60.0);
    if (direction == 'S' || direction == 'W') decimal = -decimal;
    return decimal;
}

static bool parseGGA(char *sentence)
{
    if (sentence == NULL) return false;

    char *fields[15];
    int idx = 0;
    char *token = strtok(sentence, ",");
    while (token != NULL && idx < 15)
    {
        fields[idx++] = token;
        token = strtok(NULL, ",");
    }
    if (idx < 7) return false;

    int fix_quality = atoi(fields[6]);
    if (fix_quality == 0) return false;   // no fix

    g_gps_lat = nmeaToDecimal(fields[2], fields[3][0]);
    g_gps_lon = nmeaToDecimal(fields[4], fields[5][0]);
    g_gps_timestamp = millis() / 1000;
    g_gps_valid = true;
    return true;
}

#if HW_GPS_MOCK
// Simulated fix — jitters slightly around a fixed point so repeated
// packets aren't bit-identical, useful for bench-testing the receiver.
static void hal_gps_mock_fix()
{
    g_gps_lat = 22.864000 + ((float)(random(-50, 50)) / 100000.0f);
    g_gps_lon = 88.401000 + ((float)(random(-50, 50)) / 100000.0f);
    g_gps_timestamp = millis() / 1000;
    g_gps_valid = true;
    delay(500); // simulate acquisition latency
}
#endif

// Returns true on a valid fix within GPS_FIX_TIMEOUT_MS, false on timeout.
static bool hal_gps_acquire_fix()
{
    g_gps_valid = false;

#if HW_GPS_MOCK
    hal_gps_mock_fix();
    return g_gps_valid;
#else
    unsigned long start = millis();
    char sentence[150];
    uint16_t pos = 0;

    while ((millis() - start) < GPS_FIX_TIMEOUT_MS)
    {
        while (GPS_SERIAL.available())
        {
            char c = GPS_SERIAL.read();
            if (c == '\n')
            {
                sentence[pos] = '\0';
                if (strstr(sentence, "$GNGGA") || strstr(sentence, "$GPGGA"))
                {
                    if (parseGGA(sentence)) return true;
                }
                pos = 0;
            }
            else if (pos < sizeof(sentence) - 1)
            {
                sentence[pos++] = c;
            }
        }
        delay(5);
    }
    return false; // timed out — no fix
#endif
}

// =====================================================================
// HAL: battery
// =====================================================================
static inline uint16_t hal_battery_read_mv()
{
    return api.system.bat.get();
}

// =====================================================================
// PACKET ENCODING
// =====================================================================
static inline void writeLE32(uint8_t *buf, int32_t value)
{
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
    buf[2] = (value >> 16) & 0xFF;
    buf[3] = (value >> 24) & 0xFF;
}

static inline void writeLE16(uint8_t *buf, uint16_t value)
{
    buf[0] = value & 0xFF;
    buf[1] = (value >> 8) & 0xFF;
}

static uint8_t buildPacket(uint8_t *packet, bool motionWake)
{
    int32_t lat_e6 = (int32_t)(g_gps_lat * 1000000.0);
    int32_t lon_e6 = (int32_t)(g_gps_lon * 1000000.0);

    packet[0] = DEVICE_ID;
    packet[1] = PROTOCOL_VERSION;

    packet[2] = 0;
    if (g_gps_valid)  packet[2] |= (1 << 0);
    if (motionWake)   packet[2] |= (1 << 1);

    writeLE32(&packet[3], lat_e6);
    writeLE32(&packet[7], lon_e6);
    writeLE32(&packet[11], (int32_t)g_gps_timestamp);
    writeLE16(&packet[15], g_battery_mv);
    packet[17] = g_sequence++;   // wraps at 256 by design (uint8_t)

    return PACKET_LEN;
}

// =====================================================================
// HAL: LoRa P2P
// =====================================================================
static void loraSendCallback(void)
{
    Serial.println("[HAL] LoRa TX callback fired");
}

static void hal_lora_init()
{
#if !HW_LORA_MOCK
    api.lora.nwm.set();                     // P2P mode
    api.lora.pfreq.set(LORA_FREQUENCY);
    api.lora.psf.set(LORA_SPREADING_FACTOR);
    api.lora.pcr.set(LORA_CODING_RATE);
    api.lora.ppl.set(LORA_PREAMBLE);
    api.lora.ptp.set(LORA_TX_POWER);
    api.lora.registerPSendCallback(loraSendCallback);
#endif
    Serial.println("[HAL] LoRa P2P initialized (868 MHz)");
}

static bool hal_lora_send(uint8_t *packet, uint8_t len)
{
#if HW_LORA_MOCK
    Serial.print("[MOCK TX] ");
    for (uint8_t i = 0; i < len; i++) Serial.printf("%02X ", packet[i]);
    Serial.println();
    return true;
#else
    return api.lora.psend(len, packet, false);
#endif
}

static bool transmitLocation(bool motionWake)
{
    uint8_t packet[PACKET_LEN];
    uint8_t len = buildPacket(packet, motionWake);

    Serial.println("Sending LoRa P2P packet...");
    bool ok = hal_lora_send(packet, len);
    Serial.println(ok ? "LoRa packet queued" : "LoRa packet failed");
    return ok;
}

// =====================================================================
// EVENT HANDLERS  (run outside ISR context)
// =====================================================================

// Fired ~10 ms after a motion interrupt (deferred from the ISR).
void motionHandler(void *)
{
    if (!g_motion_flag) return;
    g_motion_flag = false;

    hal_accel_clear_int();

    // Debounce: ignore motion wakes that arrive within the cooldown
    // window of the last motion-triggered transmission, so continuous
    // movement doesn't hammer the radio/battery. The 30-min periodic
    // fix (below) still runs independently and is unaffected.
    uint32_t now = millis();
    if ((now - g_last_motion_tx_ms) < MOTION_COOLDOWN_MS)
    {
        Serial.println("Motion detected (debounced, skipping TX)");
        return;
    }

    Serial.println("Motion detected -> acquiring GPS...");

    if (hal_gps_acquire_fix())
    {
        Serial.printf("GPS fix: %.6f, %.6f\n", g_gps_lat, g_gps_lon);
        g_battery_mv = hal_battery_read_mv();
        transmitLocation(true);
        g_last_motion_tx_ms = now;
    }
    else
    {
        Serial.println("GPS fix failed (motion event)");
    }
}

// Fired every MAX_FIX_INTERVAL_MS regardless of motion.
void periodicHandler(void *)
{
    Serial.println("Periodic (30 min) location update");

    if (hal_gps_acquire_fix())
    {
        g_battery_mv = hal_battery_read_mv();
        transmitLocation(false);
    }
    else
    {
        Serial.println("Periodic GPS fix failed");
    }
}

// =====================================================================
// SETUP / LOOP
// =====================================================================
void setup()
{
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("===============================");
    Serial.println(" PAW PET TRACKER - Collar Firmware");
    Serial.println(" LoRa P2P - 868 MHz");
    Serial.printf(" GPS mock: %s | LoRa mock: %s\n",
                   HW_GPS_MOCK ? "ON" : "OFF",
                   HW_LORA_MOCK ? "ON" : "OFF");
    Serial.println("===============================");

    Wire.begin();
    GPS_SERIAL.begin(GPS_BAUD);

    hal_accel_init();
    hal_lora_init();

    // One-shot timer: deferred motion handling (fires shortly after ISR)
    api.system.timer.create(RAK_TIMER_1, motionHandler, RAK_TIMER_ONESHOT);

    // Periodic timer: guarantees a fix at least every MAX_FIX_INTERVAL_MS
    api.system.timer.create(RAK_TIMER_0, periodicHandler, RAK_TIMER_PERIODIC);
    api.system.timer.start(RAK_TIMER_0, MAX_FIX_INTERVAL_MS, NULL);

#if defined(_VARIANT_RAK3172_) || defined(_VARIANT_RAK3172_SIP_)
    api.system.lpmlvl.set(2);   // deepest low-power mode the RAK3172 supports
#endif
    api.system.lpm.set(1);      // enable low power management

    Serial.println("System initialized, entering low power...");
}

void loop()
{
    // Everything is interrupt/timer driven — the MCU spends essentially
    // all of its time here, asleep.
    api.system.sleep.all();
}
