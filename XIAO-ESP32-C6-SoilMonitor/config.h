#pragma once
#include "adc_reader.h"   // defines SensorAdcConfig / AdcSource

// =============================================================================
// XIAO ESP32-C6 Soil Moisture Monitor — Configuration
// =============================================================================

// ── Sensor Count ──────────────────────────────────────────────────
// Number of sensors physically connected (1–9).
#define NUM_SENSORS 2

// ── Sensor ADC Sources ──────────────────────────────────────────────────
// Map each sensor slot (0-based) to an ADC source.
// All 10 entries must be present; only the first NUM_SENSORS are used.
//
//  ADC_ONBOARD  →  { ADC_ONBOARD, pin,   0,       0     }
//  ADC_ADS1115  →  { ADC_ADS1115, 0,     i2cAddr, chan  }
//
// XIAO ESP32-C6 actual pin → GPIO mapping (from variants/XIAO_ESP32C6/pins_arduino.h):
//   A0/D0 = GPIO0,  A1/D1 = GPIO1,  A2/D2 = GPIO2
//   D4/SDA = GPIO22,  D5/SCL = GPIO23  (reserved for I2C when ADS1115 is used)
//   Available onboard ADC pins: A0 (GPIO0), A1 (GPIO1), A2 (GPIO2)
//
// ADS1115 I2C addresses (set via ADDR pin):
//   ADDR→GND = 0x48,  ADDR→VDD = 0x49,  ADDR→SDA = 0x4A,  ADDR→SCL = 0x4B
// ───────────────────────────────────────────────────────────────────────────
static const SensorAdcConfig SENSOR_ADC_CONFIG[10] = {
    //          source        pin  i2cAddr  chan
    { ADC_ONBOARD,  A1,  0,       0 },  // S1: onboard A1 (GPIO1)
    { ADC_ONBOARD,  A2,  0,       0 },  // S2: onboard A2 (GPIO2)
    { ADC_ADS1115,  0,   0x48,    0 },  // S3: ADS1115 #1 (0x48) ch0  – expansion slot
    { ADC_ADS1115,  0,   0x48,    1 },  // S4: ADS1115 #1 (0x48) ch1
    { ADC_ADS1115,  0,   0x48,    2 },  // S5: ADS1115 #1 (0x48) ch2
    { ADC_ADS1115,  0,   0x48,    3 },  // S6: ADS1115 #1 (0x48) ch3
    { ADC_ADS1115,  0,   0x49,    0 },  // S7: ADS1115 #2 (0x49) ch0
    { ADC_ADS1115,  0,   0x49,    1 },  // S8: ADS1115 #2 (0x49) ch1
    { ADC_ADS1115,  0,   0x49,    2 },  // S9: ADS1115 #2 (0x49) ch2
    { ADC_ADS1115,  0,   0x49,    3 },  // S10: ADS1115 #2 (0x49) ch3

};

// ── Moisture Calibration Factory Defaults ────────────────────────────────────
// These values are used ONLY when no calibration has been written for a sensor.
// To calibrate without reflashing: use the zigbee2mqtt developer console to
// write cluster 0xFC11, attribute 0x0001 (dry) or 0x0002 (wet) on each endpoint.
// Values are persisted per-sensor in NVS and survive power cycles.
// See calibration.h for details.
#define CAL_DRY_DEFAULT  2200    // Factory fallback: ADC in dry air  →  0%  (3.3 V supply)
#define CAL_WET_DEFAULT   900    // Factory fallback: ADC in water    → 100%  (3.3 V supply)

// ── Battery Monitoring ────────────────────────────────────────────────────────
// IMPORTANT: Do NOT connect the 4.2 V battery directly to an ADC pin.
// Use a resistor voltage divider, e.g.:
//   Vbat ──[100 kΩ]──┬──[100 kΩ]── GND
//                     └── BATTERY_ADC_PIN
// This halves the voltage so 4.2 V → 2.1 V, safely within the 3.3 V ADC range.
// NOTE: A1 (GPIO1) and A2 (GPIO2) are used for soil sensors S1/S2.
// Battery uses A0 (GPIO0) which is free.
#define BATTERY_ADC_PIN        A0       // GPIO0 – free onboard ADC pin
#define BATTERY_DIVIDER_RATIO  2.0f     // Vbat = Vmeasured × ratio  (100k/100k = 2)
#define BATTERY_FULL_MV        4200     // mV at 100%  (fully charged 18650)
#define BATTERY_EMPTY_MV       3000     // mV at   0%  (safe cutoff for 18650)
// ADC_MAX_VALUE and ADC_REF_MV removed – battery now uses analogReadMilliVolts()
// which applies the ESP-IDF factory calibration curve internally.

// ── Sensor Power Control ──────────────────────────────────────────────────────
// NPN low-side switch (e.g. BC337-40, 800 mA, hFE 250 – supports 8+ sensors):
//
//   All sensor VCC ──────────── 3.3 V
//   All sensor GND ─── BC337 collector
//   ADS1115 GND    ─── BC337 collector  ← also switched (saves ~200 μA during sleep)
//   ADS1115 VDD    ─── 3.3 V  (always)
//   BC337 emitter  ─── board GND
//   D6 ──[10 kΩ]───────── BC337 base
//   BC337 base ──[100 kΩ]── GND   ← pull-down: keeps transistor off if GPIO floats during sleep
//
//   GPIO HIGH = transistor on  = sensors + ADS1115 grounded = active
//   GPIO LOW  = transistor off = no current drawn
//   Deep sleep: GPIO floats LOW (held by pull-down) – no active driving needed
//
// NOTE: ADS1115 VDD stays at 3.3 V; only GND is switched so no extra decoupling needed.
// The ADS1115 is re-initialised automatically on each adcPowerOn() call.
//
// Change SENSOR_POWER_PIN to any free digital output on your board.
#define SENSOR_POWER_PIN        D6    // GPIO controlling BC337 base (via 10 kΩ)
#define SENSOR_POWER_SETTLE_MS  50    // ms after power-on before reading (sensor + ADS1115 stabilise)

// ── ADC Sampling ──────────────────────────────────────────────────────────────
#define ADC_SAMPLES   10   // Readings to average per sensor/battery sample

// ── Sleep / Timing ────────────────────────────────────────────────────────────
#define SLEEP_DURATION_SEC      900    // Seconds between wake-ups (default: 15 min)
#define ZIGBEE_JOIN_TIMEOUT_MS  30000  // ms to wait for Zigbee join before sleeping
#define OTA_CHECK_WINDOW_MS     15000  // ms to wait for OTA push after each connect

// ── OTA Firmware Version ──────────────────────────────────────────────────────
// Format:  0xMAJOR MINOR PATCH BUILD  (each byte = one field)
// Increment OTA_RUNNING_VERSION in every release so the device accepts the update.
// The OTA image file created by tools/create_ota_image.py must carry a version
// number HIGHER than the value compiled into the running firmware.
#define OTA_RUNNING_VERSION    0x01030103u   // v1.3.1.3 – native genPollCtrl cluster (fixes UNSUPPORTED_CLUSTER)
#define OTA_HW_VERSION         0x0101u       // hardware revision (major.minor)

// ── Zigbee Device Identity ────────────────────────────────────────────────────
#define ZIGBEE_MANUFACTURER   "JLT"
#define ZIGBEE_MODEL          "ZB-SoilMeasure"
