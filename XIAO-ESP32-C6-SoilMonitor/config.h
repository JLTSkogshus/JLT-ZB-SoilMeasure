#pragma once
#include "adc_reader.h"   // defines SensorAdcConfig / AdcSource

// =============================================================================
// XIAO ESP32-C6 Soil Moisture Monitor — Configuration
// =============================================================================

// ── Sensor Count ──────────────────────────────────────────────────
// Number of sensors physically connected (1–9).
#define NUM_SENSORS 3

// ── Sensor ADC Sources ──────────────────────────────────────────────────
// Map each sensor slot (0-based) to an ADC source.
// All 9 entries must be present; only the first NUM_SENSORS are used.
//
//  ADC_ONBOARD  →  { ADC_ONBOARD, pin,   0,       0     }
//  ADC_ADS1115  →  { ADC_ADS1115, 0,     i2cAddr, chan  }
//
// XIAO ESP32-C6 I2C: SDA=D4(A4/GPIO6), SCL=D5(A5/GPIO7)
//   → A4 and A5 are NOT available as analog inputs when I2C is active.
//   Available onboard ADC pins: A0 (GPIO2), A1 (GPIO3), A2 (GPIO4), A3 (GPIO5)
//
// ADS1115 I2C addresses (set via ADDR pin):
//   ADDR→GND = 0x48,  ADDR→VDD = 0x49,  ADDR→SDA = 0x4A,  ADDR→SCL = 0x4B
// ───────────────────────────────────────────────────────────────────────────
static const SensorAdcConfig SENSOR_ADC_CONFIG[9] = {
    //          source        pin  i2cAddr  chan
    { ADC_ADS1115,  0,   0x48,    0 },  // S1: ADS1115 #1 (0x48) ch0
    { ADC_ADS1115,  0,   0x48,    1 },  // S2: ADS1115 #1 (0x48) ch1
    { ADC_ONBOARD,  A1,  0,       0 },  // S3: onboard A1 (GPIO3)
    { ADC_ADS1115,  0,   0x48,    2 },  // S4: ADS1115 #1 (0x48) ch2
    { ADC_ADS1115,  0,   0x48,    3 },  // S5: ADS1115 #1 (0x48) ch3
    { ADC_ADS1115,  0,   0x49,    0 },  // S6: ADS1115 #2 (0x49) ch0
    { ADC_ADS1115,  0,   0x49,    1 },  // S7: ADS1115 #2 (0x49) ch1
    { ADC_ADS1115,  0,   0x49,    2 },  // S8: ADS1115 #2 (0x49) ch2
    { ADC_ONBOARD,  A2,  0,       0 },  // S9: onboard A2 (GPIO4)
};

// ── Moisture Calibration Factory Defaults ────────────────────────────────────
// These values are used ONLY when no calibration has been written for a sensor.
// To calibrate without reflashing: use the zigbee2mqtt developer console to
// write cluster 0xFC11, attribute 0x0001 (dry) or 0x0002 (wet) on each endpoint.
// Values are persisted per-sensor in NVS and survive power cycles.
// See calibration.h for details.
#define CAL_DRY_DEFAULT  3200    // Factory fallback: ADC in dry air  →  0%
#define CAL_WET_DEFAULT  1400    // Factory fallback: ADC in water    → 100%

// ── Battery Monitoring ────────────────────────────────────────────────────────
// IMPORTANT: Do NOT connect the 4.2 V battery directly to an ADC pin.
// Use a resistor voltage divider, e.g.:
//   Vbat ──[100 kΩ]──┬──[100 kΩ]── GND
//                     └── BATTERY_ADC_PIN
// This halves the voltage so 4.2 V → 2.1 V, safely within the 3.3 V ADC range.
// NOTE: A4 (GPIO6/SDA) and A5 (GPIO7/SCL) are reserved for I2C (ADS1115).
// Battery must use A0, A1, A2, or A3.  Default: A0.
#define BATTERY_ADC_PIN        A0       // GPIO2 – free onboard ADC pin
#define BATTERY_DIVIDER_RATIO  2.0f     // Vbat = Vmeasured × ratio  (100k/100k = 2)
#define BATTERY_FULL_MV        4200     // mV at 100%  (fully charged 18650)
#define BATTERY_EMPTY_MV       3000     // mV at   0%  (safe cutoff for 18650)
#define ADC_MAX_VALUE          4095.0f  // 12-bit ADC (ESP32-C6 default)
#define ADC_REF_MV             3300.0f  // ADC reference voltage (3.3 V rail)

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
#define OTA_RUNNING_VERSION    0x01010000u   // v1.1.0.0 – current firmware
#define OTA_HW_VERSION         0x0101u       // hardware revision (major.minor)

// ── Zigbee Device Identity ────────────────────────────────────────────────────
#define ZIGBEE_MANUFACTURER   "JLT"
#define ZIGBEE_MODEL          "ZB-SoilMeasure"
