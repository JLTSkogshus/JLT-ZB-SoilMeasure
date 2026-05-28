#pragma once

// =============================================================================
// Per-sensor calibration – stored in NVS, writable over Zigbee
// =============================================================================
//
// Custom Zigbee cluster: 0xFC11  (manufacturer-specific, server-side)
//   Attribute 0x0001 : Dry ADC  (uint16, read/write) – raw ADC in dry air   → 0%
//   Attribute 0x0002 : Wet ADC  (uint16, read/write) – raw ADC in water     → 100%
//
// Write from zigbee2mqtt dev console (or MQTT):
//   cluster  : 0xFC11
//   attribute: 0x0001  (dry) or 0x0002 (wet)
//   value    : raw ADC integer (read from Serial Monitor while sensor is dry/wet)
//
// Changes persist in NVS and survive power cycles and firmware updates.
// Factory defaults are defined in config.h as CAL_DRY_DEFAULT / CAL_WET_DEFAULT.
// =============================================================================

#include <Preferences.h>
#include "config.h"

// ── Zigbee cluster / attribute IDs ────────────────────────────────────────────
#define CAL_CLUSTER_ID   0xFC11u   // Manufacturer-specific calibration cluster
#define CAL_ATTR_DRY          0x0001u   // uint16 R/W – ADC when sensor is dry
#define CAL_ATTR_WET          0x0002u   // uint16 R/W – ADC when sensor is in water
#define CAL_ATTR_SLEEP        0x0003u   // uint32 R/W – deep-sleep duration in seconds
#define CAL_ATTR_SLEEP_ENABLE 0x0004u   // uint8  R/W – 1 = deep-sleep on, 0 = stay awake (default)
#define CAL_ATTR_RAW_ADC      0x0005u   // uint16 R   – last raw ADC reading (updated each report cycle)

struct SensorCalibration {
    uint16_t dryAdc;   // ADC reading in dry air  → 0% moisture
    uint16_t wetAdc;   // ADC reading in water    → 100% moisture
};

class CalibrationManager {
public:
    // Optional explicit init – also called automatically on first get()/set().
    void begin();

    // Read calibration for sensor index 0–8.
    // Returns factory defaults (CAL_DRY_DEFAULT / CAL_WET_DEFAULT) if never set.
    SensorCalibration get(uint8_t sensorIdx);

    // Persist new calibration for sensor index 0–8.
    void set(uint8_t sensorIdx, const SensorCalibration &cal);

    // Erase all stored calibration → reverts to factory defaults on next read.
    void resetToDefaults();

    // Read / write device-wide deep-sleep interval (seconds, default SLEEP_DURATION_SEC).
    uint32_t getSleepSeconds();
    void     setSleepSeconds(uint32_t seconds);

    // Read / write sleep-mode enable flag (default: false = stay awake).
    // Write 1 (true) to enable deep-sleep between readings.
    bool getSleepEnabled();
    void setSleepEnabled(bool enabled);

private:
    Preferences _prefs;
    bool        _initialized = false;
    void        ensureInit();
};

extern CalibrationManager Calibration;
