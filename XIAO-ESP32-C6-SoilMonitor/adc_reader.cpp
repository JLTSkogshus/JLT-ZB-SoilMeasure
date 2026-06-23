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

// Read a single raw sample (one conversion) for the given sensor index.
// Returns a 12-bit normalised value (0–4095).
static uint16_t adcSampleOnce(uint8_t sensorIdx) {
    const SensorAdcConfig &cfg = SENSOR_ADC_CONFIG[sensorIdx];

    if (cfg.source == ADC_ONBOARD) {
        return (uint16_t)analogRead(cfg.pin);   // 12-bit, 0–4095
    }

    // ── ADS1115 ──────────────────────────────────────────────────────────────
    int idx = addrIdx(cfg.i2cAddr);
    if (!s_adsReady[idx]) return 0u;

    int16_t raw = s_ads[idx].readADC_SingleEnded(cfg.channel);
    // Normalise 16-bit (0–32767 at GAIN_ONE) → 12-bit (0–4095)
    return (raw > 0) ? (uint16_t)((uint32_t)raw >> 3u) : 0u;
}

// Simple insertion sort for small arrays.
static void sortU16(uint16_t *arr, int n) {
    for (int i = 1; i < n; i++) {
        uint16_t key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) { arr[j + 1] = arr[j]; j--; }
        arr[j + 1] = key;
    }
}

// Read all `count` sensors using ADC_SAMPLES interleaved rounds.
// Each round samples every sensor once before moving to the next round,
// spreading the readings evenly across time so noise from one moment does
// not bias a single sensor.
// After collecting ADC_SAMPLES readings per sensor the values are sorted and
// the middle ADC_TRIM_SAMPLES readings are averaged (trimmed mean), discarding
// the lowest and highest outliers.
// Results (12-bit, 0–4095) are written into `results[0..count-1]`.
void adcReadAllRaw(uint16_t *results, uint8_t count) {
    // samples[sensor][round]
    uint16_t samples[NUM_SENSORS][ADC_SAMPLES];

    for (int round = 0; round < ADC_SAMPLES; round++) {
        for (int s = 0; s < count; s++) {
            samples[s][round] = adcSampleOnce(s);
        }
        delay(ADC_ROUND_DELAY_MS);   // brief pause between rounds
    }

    const int trim = (ADC_SAMPLES - ADC_TRIM_SAMPLES) / 2;  // samples to drop from each end
    for (int s = 0; s < count; s++) {
        sortU16(samples[s], ADC_SAMPLES);
        uint32_t sum = 0;
        for (int r = trim; r < trim + ADC_TRIM_SAMPLES; r++) {
            sum += samples[s][r];
        }
        results[s] = (uint16_t)(sum / ADC_TRIM_SAMPLES);
    }
}

// Legacy single-sensor read – kept for battery and other one-off reads.
uint16_t adcReadRaw(uint8_t sensorIdx) {
    return adcSampleOnce(sensorIdx);
}
