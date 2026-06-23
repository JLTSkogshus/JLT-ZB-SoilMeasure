#pragma once
#include <Arduino.h>

// =============================================================================
// ADC abstraction – supports onboard ESP32-C6 ADC and external ADS1115 over I2C
// =============================================================================
//
// ADS1115 notes
// ─────────────────────────────────────────────────────────────────────────────
//  • 16-bit I2C ADC, 4 single-ended channels per chip
//  • I2C addresses (set via ADDR pin):
//      ADDR → GND = 0x48   ADDR → VDD = 0x49
//      ADDR → SDA = 0x4A   ADDR → SCL = 0x4B
//  • Up to 4 chips per I2C bus → 16 sensor channels total
//
// IMPORTANT – I2C pins on XIAO ESP32-C6
// ─────────────────────────────────────────────────────────────────────────────
//  SDA = D4 = GPIO6 = A4
//  SCL = D5 = GPIO7 = A5
//  A4 and A5 are NOT available as analog inputs when I2C is active.
//  Available onboard ADC pins: A0 (GPIO2), A1 (GPIO3), A2 (GPIO4), A3 (GPIO5)
//
// Output normalization
// ─────────────────────────────────────────────────────────────────────────────
//  adcReadRaw() always returns a 12-bit equivalent value (0–4095), regardless
//  of source.  ADS1115 raw 16-bit values (0–32767 at GAIN_ONE ±4.096 V) are
//  right-shifted by 3 bits so the same calibration numbers work for both ADC
//  types without any conversion.
// =============================================================================

enum AdcSource : uint8_t {
    ADC_ONBOARD = 0,   // Built-in ESP32-C6 12-bit ADC
    ADC_ADS1115 = 1,   // External I2C ADS1115 / ADS1015
};

struct SensorAdcConfig {
    AdcSource source;
    int       pin;       // ADC_ONBOARD: Arduino analog pin (A0–A3)
    uint8_t   i2cAddr;   // ADC_ADS1115: I2C address (0x48–0x4B)
    uint8_t   channel;   // ADC_ADS1115: channel 0–3
};

// Call once from setup() – initialises Wire and all unique ADS1115 chips
// declared in SENSOR_ADC_CONFIG.  Also configures SENSOR_POWER_PIN as output
// and leaves sensors powered OFF.
void adcReaderBegin();

// Drive SENSOR_POWER_PIN HIGH and wait SENSOR_POWER_SETTLE_MS.
// Call immediately before adcReadRaw().
void adcPowerOn();

// Drive SENSOR_POWER_PIN LOW – cuts sensor VCC during deep sleep.
void adcPowerOff();

// Read all `count` sensors (0-based, first count entries in SENSOR_ADC_CONFIG)
// using ADC_SAMPLES interleaved rounds and a trimmed mean of the middle
// ADC_TRIM_SAMPLES readings.  Results are written into results[0..count-1].
// Returns 0–4095 (12-bit normalised) per sensor.
void adcReadAllRaw(uint16_t *results, uint8_t count);

// Legacy single-sample read for one sensor (no averaging, no trimming).
// Still used for battery reads and other one-off conversions.
uint16_t adcReadRaw(uint8_t sensorIdx);
