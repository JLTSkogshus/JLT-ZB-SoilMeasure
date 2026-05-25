/**
 * XIAO ESP32-C6 Soil Moisture Monitor
 *
 * Hardware
 * ─────────────────────────────────────────────────────────────────────────────
 *  • Seeed Studio XIAO ESP32-C6
 *  • 1–9 × Capacitive Soil Moisture Sensor V1.2
 *  • 3.7 V 18650 2000 mAh Li-Ion battery
 *  • Resistor voltage divider for battery ADC (see config.h)
 *
 * Zigbee clusters exposed (per sensor endpoint 1–N)
 * ─────────────────────────────────────────────────────────────────────────────
 *  • Basic                   (0x0000) – manufacturer, model, SW version
 *  • Power Configuration     (0x0001) – battery % (reported on endpoint 1)
 *  • Relative Humidity       (0x0405) – soil moisture mapped as RH%
 *  • Calibration (0xFC11)             – dry ADC (attr 0x0001) + wet ADC (attr 0x0002)
 *                                       writable per endpoint from zigbee2mqtt dev console
 *
 * Home Assistant / zigbee2mqtt
 * ─────────────────────────────────────────────────────────────────────────────
 *  Each sensor endpoint appears as a "humidity" sensor entity.
 *  Rename them in HA to "Soil Moisture – Pot 1", etc.
 *  Battery % is available as a separate entity on the first endpoint.
 *
 * Board settings (Arduino IDE / VS Code Arduino extension)
 * ─────────────────────────────────────────────────────────────────────────────
 *  Board   : Seeed Studio XIAO ESP32C6
 *  Package : esp32 by Espressif  ≥ 3.0.0
 *  Zigbee  : Tools → Zigbee Mode → Zigbee ED (end device)   ← REQUIRED
 *
 * Deep-sleep behaviour
 * ─────────────────────────────────────────────────────────────────────────────
 *  1. Wake from timer (or power-on)
 *  2. Connect to Zigbee coordinator (join if first boot)
 *  3. Read all sensors + battery
 *  4. Report via Zigbee
 *  5. Return to deep sleep for SLEEP_DURATION_SEC
 *
 *  All state that must survive deep sleep is stored in RTC memory.
 */

// ── Zigbee mode guard ────────────────────────────────────────────────────────
#ifndef ZIGBEE_MODE_ED
  #error "Select 'Zigbee ED (end device)' under Tools → Zigbee Mode in Arduino IDE"
#endif

#include <Arduino.h>
#include <Zigbee.h>
#include <esp_sleep.h>
#include "config.h"              // includes adc_reader.h
#include "calibration.h"
#include "adc_reader.h"
#include "zigbee_soil_sensor.h"  // ZigbeeSoilSensor (extends ZigbeeTempSensor)

// =============================================================================
// RTC memory – persists across deep sleep
// =============================================================================
RTC_DATA_ATTR static uint32_t s_bootCount         = 0;   // total wake-ups
RTC_DATA_ATTR static uint32_t s_lastConnectionSec = 0;   // seconds since power-on at last TX
RTC_DATA_ATTR static uint32_t s_nextWakeupSec     = 0;   // seconds since power-on at next wake

// =============================================================================
// Zigbee endpoints – one ZigbeeSoilSensor per sensor slot (endpoint IDs 1–9)
// Each endpoint exposes: Basic, Power Config, Temperature (unused),
// Relative Humidity (moisture %), and calibration cluster 0xFC11.
// Only the first NUM_SENSORS endpoints are registered with the Zigbee stack.
// =============================================================================
static ZigbeeSoilSensor zbSoil0(1,0), zbSoil1(2,1), zbSoil2(3,2),
                        zbSoil3(4,3), zbSoil4(5,4), zbSoil5(6,5),
                        zbSoil6(7,6), zbSoil7(8,7), zbSoil8(9,8);

static ZigbeeSoilSensor* const zbSoils[9] = {
  &zbSoil0, &zbSoil1, &zbSoil2,
  &zbSoil3, &zbSoil4, &zbSoil5,
  &zbSoil6, &zbSoil7, &zbSoil8
};

// =============================================================================
// Forward declarations
// =============================================================================
static float    readMoisturePercent(uint8_t sensorIdx);
static uint16_t readBatteryMillivolts();
static uint8_t  readBatteryPercent();
static void     reportAllSensors();
static void     enterDeepSleep(uint32_t seconds);

// =============================================================================
// OTA state
// =============================================================================
static volatile bool s_otaActive = false;

static void onOtaState(bool active) {
  s_otaActive = active;
  Serial.printf("[OTA] %s\n",
    active ? "Download started \u2013 deep sleep disabled"
           : "Download complete \u2013 rebooting");
}

// =============================================================================
// setup() – runs once after every wake-up
// =============================================================================
void setup() {
  Serial.begin(115200);
  s_bootCount++;

  Serial.printf("\n=== JLT ZB-SoilMeasure  boot #%lu ===\n", (unsigned long)s_bootCount);

  // ── Initialise ADC backends (onboard + ADS1115 over I2C) ────────────────────────
  adcReaderBegin();

  // ── Open NVS calibration store ───────────────────────────────────────────────
  Calibration.begin();

  // ── Configure Zigbee endpoints ──────────────────────────────────────────────  // OTA client lives on endpoint 1 (zbSoil0) – one per device is sufficient.
  zbSoil0.addOTAClient(OTA_RUNNING_VERSION, OTA_RUNNING_VERSION, OTA_HW_VERSION);
  zbSoil0.onOTAStateChange(onOtaState);
  for (int i = 0; i < NUM_SENSORS; i++) {
    zbSoils[i]->setManufacturerAndModel(ZIGBEE_MANUFACTURER, ZIGBEE_MODEL);
    zbSoils[i]->setVersion((OTA_RUNNING_VERSION >> 24) & 0xFF);  // major version → Basic cluster AppVersion
    zbSoils[i]->setPowerSource(ZB_POWER_SOURCE_BATTERY, readBatteryPercent());
    Zigbee.addEndpoint(zbSoils[i]);
  }

  // ── Start Zigbee stack as End Device ────────────────────────────────────────
  esp_zb_cfg_t zbConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  Zigbee.begin(&zbConfig, /*erase_nvs=*/false);

  // ── Wait for network join ────────────────────────────────────────────────────
  Serial.println("Connecting to Zigbee network...");
  uint32_t joinStart = millis();
  while (!Zigbee.connected()) {
    if (millis() - joinStart > ZIGBEE_JOIN_TIMEOUT_MS) {
      Serial.println("Join timed out – going back to sleep.");
      enterDeepSleep(SLEEP_DURATION_SEC);
    }
    delay(100);
  }
  Serial.println("Zigbee connected!");

  // ── Record connection timestamp ──────────────────────────────────────────────
  s_lastConnectionSec = millis() / 1000;

  // ── Read and transmit sensor data ────────────────────────────────────────────
  reportAllSensors();
  // ── Check for OTA firmware update ────────────────────────────────
  // requestOTAUpdate() sends a Query Next Image Request to the coordinator.
  // The coordinator (zigbee2mqtt) will respond with image info if an update is
  // staged.  We then wait OTA_CHECK_WINDOW_MS for the transfer to begin.
  zbSoil0.requestOTAUpdate();
  Serial.printf("[OTA] Checking for update (%u ms window)...\n", OTA_CHECK_WINDOW_MS);
  {
    uint32_t t0 = millis();
    while (!s_otaActive && (millis() - t0) < OTA_CHECK_WINDOW_MS) {
      delay(100);
    }
  }
  if (s_otaActive) {
    Serial.println("[OTA] In progress \u2013 blocking deep sleep until done.");
    while (s_otaActive) {
      delay(1000);
    }
    Serial.println("[OTA] Done \u2013 rebooting with new firmware.");
    delay(2000);
    ESP.restart();
  }
  Serial.println("[OTA] No update available.");
  // ── Sleep ────────────────────────────────────────────────────────────────────
  // Use NVS value so it can be changed over Zigbee (cluster 0xFC11 attr 0x0003).
  enterDeepSleep(Calibration.getSleepSeconds());
}

// loop() is never reached – device always sleeps at end of setup()
void loop() {}

// =============================================================================
// readMoisturePercent()
// Returns 0.0–100.0 % for sensor at sensorIdx.
// Reads via adcReadRaw() (onboard or ADS1115, normalised to 12-bit scale).
// Calibration loaded from NVS; writable per-sensor over Zigbee cluster 0xFC11.
// =============================================================================
static float readMoisturePercent(uint8_t sensorIdx) {
  SensorCalibration cal = Calibration.get(sensorIdx);
  uint16_t raw     = adcReadRaw(sensorIdx);   // 0–4095, averaged, normalised
  float moisture   = (cal.dryAdc - (float)raw) * 100.0f
                     / (float)(cal.dryAdc - cal.wetAdc);
  return constrain(moisture, 0.0f, 100.0f);
}

// =============================================================================
// readBatteryMillivolts()
// Reads the voltage-divided battery voltage and scales it back to Vbat in mV.
// =============================================================================
static uint16_t readBatteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(BATTERY_ADC_PIN);
    delay(2);
  }
  float rawAvg  = (float)(sum / ADC_SAMPLES);
  float vmeasMv = (rawAvg / ADC_MAX_VALUE) * ADC_REF_MV;
  return (uint16_t)(vmeasMv * BATTERY_DIVIDER_RATIO);
}

// =============================================================================
// readBatteryPercent()
// Maps battery mV to 0–100 %.
// =============================================================================
static uint8_t readBatteryPercent() {
  uint16_t mv = readBatteryMillivolts();
  if (mv >= BATTERY_FULL_MV)  return 100;
  if (mv <= BATTERY_EMPTY_MV) return 0;
  return (uint8_t)(((uint32_t)(mv - BATTERY_EMPTY_MV) * 100UL)
                    / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

// =============================================================================
// reportAllSensors()
// Reads every active sensor and battery, then pushes values to the coordinator.
// =============================================================================
static void reportAllSensors() {
  uint8_t  battPct = readBatteryPercent();
  uint16_t battMv  = readBatteryMillivolts();

  Serial.printf("Battery : %3d %%  (%d mV)\n", battPct, battMv);
  Serial.printf("Last connection : %lu s  |  Next wakeup : %lu s  |  Boot : %lu\n",
                (unsigned long)s_lastConnectionSec,
                (unsigned long)s_nextWakeupSec,
                (unsigned long)s_bootCount);

  for (int i = 0; i < NUM_SENSORS; i++) {
    float moisture = readMoisturePercent(i);
    SensorCalibration cal = Calibration.get(i);
    Serial.printf("Sensor %d : %.1f %%  [cal dry=%u wet=%u]\n",
                  i + 1, moisture, cal.dryAdc, cal.wetAdc);

    zbSoils[i]->setHumidity(moisture);                           // moisture % → RH cluster
    zbSoils[i]->setPowerSource(ZB_POWER_SOURCE_BATTERY, battPct); // battery % on each EP
    zbSoils[i]->reportHumidity();
  }
}

// =============================================================================
// enterDeepSleep()
// Commits the next-wakeup timestamp to RTC memory, then sleeps.
// =============================================================================
static void enterDeepSleep(uint32_t seconds) {
  s_nextWakeupSec = (millis() / 1000) + seconds;
  Serial.printf("Sleeping %lu s – next wakeup at %lu s elapsed\n",
                (unsigned long)seconds, (unsigned long)s_nextWakeupSec);
  Serial.flush();

  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}
