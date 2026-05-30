#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include "adc_reader.h"
#include "config.h"

// Four static ADS1115 objects – one per possible I2C address (0x48–0x4B)
static Adafruit_ADS1115 s_ads[4];
static bool             s_adsReady[4] = { false, false, false, false };

// Maps I2C address 0x48–0x4B to array index 0–3
static inline int addrIdx(uint8_t addr) { return (int)(addr - 0x48u); }

void adcReaderBegin() {
    // Configure sensor power pin – start with sensors OFF.
    pinMode(SENSOR_POWER_PIN, OUTPUT);
    digitalWrite(SENSOR_POWER_PIN, LOW);

    Wire.begin();

    for (int si = 0; si < NUM_SENSORS; si++) {
        if (SENSOR_ADC_CONFIG[si].source != ADC_ADS1115) continue;

        int     idx  = addrIdx(SENSOR_ADC_CONFIG[si].i2cAddr);
        uint8_t addr = SENSOR_ADC_CONFIG[si].i2cAddr;

        if (s_adsReady[idx]) continue;   // already initialised

        // GAIN_ONE = ±4.096 V → 1 LSB = 0.125 mV
        // Handles 3.3 V soil sensor output safely (max ~3.3 V < 4.096 V)
        s_ads[idx].setGain(GAIN_ONE);
        s_adsReady[idx] = s_ads[idx].begin(addr);

        if (s_adsReady[idx]) {
            Serial.printf("[adc] ADS1115 at 0x%02X ready\n", addr);
        } else {
            Serial.printf("[adc] ERROR: ADS1115 at 0x%02X not found – check wiring!\n", addr);
        }
    }
}

void adcPowerOn() {
    digitalWrite(SENSOR_POWER_PIN, HIGH);  // enable transistor → sensors on
    delay(SENSOR_POWER_SETTLE_MS);         // wait for sensor output to stabilise
}

void adcPowerOff() {
    digitalWrite(SENSOR_POWER_PIN, LOW);   // disable transistor → sensors draw no current
}

uint16_t adcReadRaw(uint8_t sensorIdx) {
    const SensorAdcConfig &cfg = SENSOR_ADC_CONFIG[sensorIdx];
    uint32_t sum = 0;

    if (cfg.source == ADC_ONBOARD) {
        for (int i = 0; i < ADC_SAMPLES; i++) {
            sum += (uint32_t)analogRead(cfg.pin);
            delay(2);
        }
        return (uint16_t)(sum / ADC_SAMPLES);   // 12-bit, 0–4095
    }

    // ── ADS1115 ──────────────────────────────────────────────────────────────
    int idx = addrIdx(cfg.i2cAddr);
    if (!s_adsReady[idx]) return 0u;

    for (int i = 0; i < ADC_SAMPLES; i++) {
        int16_t raw = s_ads[idx].readADC_SingleEnded(cfg.channel);
        sum += (raw > 0) ? (uint32_t)raw : 0u;
        delay(2);
    }
    // Normalise 16-bit (0–32767 at GAIN_ONE) → 12-bit (0–4095)
    // so calibration values are the same regardless of ADC source.
    return (uint16_t)((sum / ADC_SAMPLES) >> 3u);
}
