#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include "adc_reader.h"
#include "config.h"

// Four static ADS1115 objects – one per possible I2C address (0x48–0x4B)
static Adafruit_ADS1115 s_ads[4];
static bool             s_adsReady[4] = { false, false, false, false };

// Maps I2C address 0x48–0x4B to array index 0–3
static inline int addrIdx(uint8_t addr) { return (int)(addr - 0x48u); }

// (Re-)initialise every ADS1115 chip declared in SENSOR_ADC_CONFIG.
// Called from adcPowerOn() each time the transistor turns on because the
// ADS1115 loses its gain/config register when its GND rail is switched off.
static void initAdsChips() {
    for (int si = 0; si < NUM_SENSORS; si++) {
        if (SENSOR_ADC_CONFIG[si].source != ADC_ADS1115) continue;

        int     idx  = addrIdx(SENSOR_ADC_CONFIG[si].i2cAddr);
        uint8_t addr = SENSOR_ADC_CONFIG[si].i2cAddr;

        if (s_adsReady[idx]) continue;   // already done this power cycle

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

void adcReaderBegin() {
    // Configure sensor power pin – start with transistor OFF.
    // ADS1115 GND is also on the switched rail, so it is unpowered here.
    pinMode(SENSOR_POWER_PIN, OUTPUT);
    digitalWrite(SENSOR_POWER_PIN, LOW);
    Wire.begin();   // just configures the I2C peripheral – no chip communication yet
    // ADS1115 init happens inside adcPowerOn() each measurement cycle.
}

void adcPowerOn() {
    // Reset ready flags – ADS1115 loses its config register when GND is removed.
    for (int i = 0; i < 4; i++) s_adsReady[i] = false;
    digitalWrite(SENSOR_POWER_PIN, HIGH);  // NPN on → sensors + ADS1115 GND connected
    delay(SENSOR_POWER_SETTLE_MS);         // wait for chips and sensors to stabilise
    initAdsChips();                        // re-configure ADS1115 gain etc.
}

void adcPowerOff() {
    digitalWrite(SENSOR_POWER_PIN, LOW);   // NPN: base LOW → transistor off → no current drawn
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
