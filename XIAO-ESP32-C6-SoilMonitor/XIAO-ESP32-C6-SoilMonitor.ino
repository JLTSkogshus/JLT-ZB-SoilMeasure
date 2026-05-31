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
// Increase the Arduino loop-task stack (default 8 KB overflows when 3+ Zigbee
// endpoints + OTA client are active and reportHumidity() is called per sensor).
// Override the weak symbol defined in cores/esp32/main.cpp.
// =============================================================================
size_t getArduinoLoopTaskStackSize(void) { return 16 * 1024; }

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
static ZigbeeSoilSensor zbSoil0(1,0), zbSoil1(2,1);

static ZigbeeSoilSensor* const zbSoils[NUM_SENSORS] = {
  &zbSoil0, &zbSoil1
};

// Endpoint 4 – onboard user LED (active low: LOW = on, HIGH = off)
static ZigbeeLight zbLed(4);

// =============================================================================
// Forward declarations
// =============================================================================
static uint16_t readBatteryMillivolts();
static uint8_t  readBatteryPercent();
static void     reportAllSensors();
static void     enterDeepSleep(uint32_t seconds);
static void     onLedChange(bool state);

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

  // ── User LED ─────────────────────────────────────────────────────────────────
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);  // off initially (active low)

  // ── Initialise ADC backends (onboard + ADS1115 over I2C) ────────────────────────
  adcReaderBegin();

  // ── Open NVS calibration store ───────────────────────────────────────────────
  Calibration.begin();
  // Refresh ZCL attribute backing stores from NVS now that the store is open.
  // The static constructors may have run before NVS was ready, leaving the
  // ZCL attributes at their zero-initialised values.
  for (int i = 0; i < NUM_SENSORS; i++) zbSoils[i]->updateCalFromNvs();

  // ── Configure Zigbee endpoints ──────────────────────────────────────────────  // OTA client lives on endpoint 1 (zbSoil0) – one per device is sufficient.
  zbSoil0.addOTAClient(OTA_RUNNING_VERSION, OTA_RUNNING_VERSION, OTA_HW_VERSION);
  zbSoil0.onOTAStateChange(onOtaState);
  for (int i = 0; i < NUM_SENSORS; i++) {
    zbSoils[i]->setManufacturerAndModel(ZIGBEE_MANUFACTURER, ZIGBEE_MODEL);
    zbSoils[i]->setVersion((OTA_RUNNING_VERSION >> 24) & 0xFF);  // AppVersion byte
    // Seed battery % and voltage (genPowerCfg). Voltage is in 100 mV units.
    zbSoils[i]->setPowerSource(ZB_POWER_SOURCE_BATTERY, readBatteryPercent(),
                              (uint8_t)(readBatteryMillivolts() / 100));
    Zigbee.addEndpoint(zbSoils[i]);
  }
  // LED endpoint (4) – standard on/off light, controlled by coordinator
  zbLed.setManufacturerAndModel(ZIGBEE_MANUFACTURER, ZIGBEE_MODEL);
  zbLed.onLightChange(onLedChange);
  Zigbee.addEndpoint(&zbLed);

  // ── Start Zigbee stack as End Device ────────────────────────────────────────
  esp_zb_cfg_t zbConfig = ZIGBEE_DEFAULT_ED_CONFIG();
  Zigbee.begin(&zbConfig, /*erase_nvs=*/false);

  // ── Wait for network join ────────────────────────────────────────────────────
  Serial.println("Connecting to Zigbee network...");
  uint32_t joinStart = millis();
  while (!Zigbee.connected()) {
    if (millis() - joinStart > ZIGBEE_JOIN_TIMEOUT_MS) {
      if (Calibration.getSleepEnabled()) {
        Serial.println("Join timed out \u2013 going back to sleep.");
        enterDeepSleep(Calibration.getSleepSeconds());
      } else {
        // Sleep disabled (dev mode) \u2013 keep retrying instead of sleeping.
        Serial.println("Join timed out \u2013 retrying (sleep disabled).");
        joinStart = millis();
      }
    }
    delay(100);
  }
  Serial.println("Zigbee connected!");

  // ── Record connection timestamp ──────────────────────────────────────────────
  s_lastConnectionSec = millis() / 1000;

  // ── Read and transmit sensor data ────────────────────────────────────────────
  // Report immediately while the Zigbee stack is still idle.  Reporting BEFORE
  // z2m starts its ZCL interview read phase avoids a concurrent-access crash
  // inside the ESP32 Zigbee stack (heap corruption when an incoming cluster read
  // races with an outgoing attribute report on the same endpoint).
  reportAllSensors();

  // Give z2m 2 s to complete its ZDO/ZCL interview now that we are done
  // transmitting.  The Zigbee task can process all incoming requests freely
  // while the app task is blocked here (no lock contention).
  delay(2000);
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

  // ── Sleep / Awake decision ───────────────────────────────────────────────────
  // Sleep mode is controlled via cluster 0xFC11 attribute 0x0004 (sleep_enable).
  // Default: DISABLED (stays awake for development).
  // Set attr 0x0004 = 1 (or toggle the switch in z2m) to enable deep sleep.
  Serial.printf("[sleep] Sleep mode: %s\n",
                Calibration.getSleepEnabled() ? "ENABLED" : "DISABLED (awake loop)");

  if (!Calibration.getSleepEnabled()) {
    // Awake mode – report every sleep_duration seconds, never enter deep sleep.
    // The Zigbee task continues running in the background, so attribute writes
    // (e.g. toggling the sleep switch in z2m) are processed while we wait.
    Serial.println("[sleep] Staying awake – reporting every sleep_duration s. "
                   "Set attr 0x0004=1 on cluster 0xFC11 to enable sleep.");
    digitalWrite(LED_BUILTIN, LOW);   // LED on – indicates awake/dev mode
    for (;;) {
      uint32_t t0 = millis();
      // Re-evaluate sleep_duration each tick so a z2m write takes effect within 100 ms.
      while (millis() - t0 < (uint32_t)Calibration.getSleepSeconds() * 1000UL) {
        delay(100);
        if (zigbeeSoilGetReportNow()) {
          zigbeeSoilClearReportNow();
          Serial.println("[report] Immediate report triggered via Zigbee.");
          reportAllSensors();
          t0 = millis();  // reset interval so next scheduled report isn't immediate
        }
      }
      reportAllSensors();
      zbSoil0.requestOTAUpdate();
      if (Calibration.getSleepEnabled()) {
        Serial.println("[sleep] Sleep re-enabled – entering deep sleep.");
        break;
      }
    }
  }

  // ── Deep sleep ────────────────────────────────────────────────────────────────
  enterDeepSleep(Calibration.getSleepSeconds());
}

// loop() is never reached – device always sleeps at end of setup()
void loop() {}

// =============================================================================
// onLedChange()
// Called by ZigbeeLight when the coordinator sends an on/off command.
// LED_BUILTIN (GPIO15) is active low on XIAO ESP32-C6.
// =============================================================================
static void onLedChange(bool state) {
  digitalWrite(LED_BUILTIN, state ? LOW : HIGH);
  Serial.printf("[LED] User LED %s\n", state ? "ON" : "OFF");
}

// =============================================================================
// readBatteryMillivolts()
// Reads the voltage-divided battery voltage and scales it back to Vbat in mV.
// =============================================================================
static uint16_t readBatteryMillivolts() {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += (uint32_t)analogReadMilliVolts(BATTERY_ADC_PIN);
    delay(2);
  }
  uint32_t vmeasMv = sum / ADC_SAMPLES;
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

  // Power sensors on, read, then immediately power off.
  // Sensors draw no current outside this window (NPN transistor switch).
  adcPowerOn();
  for (int i = 0; i < NUM_SENSORS; i++) {
    uint16_t raw = adcReadRaw(i);            // averaged, normalised to 12-bit
    SensorCalibration cal = Calibration.get(i);
    float moisture = constrain(
        (cal.dryAdc - (float)raw) * 100.0f / (float)(cal.dryAdc - cal.wetAdc),
        0.0f, 100.0f);

    Serial.printf("Sensor %d : %.1f %%  raw=%u  [cal dry=%u wet=%u]\n",
                  i + 1, moisture, raw, cal.dryAdc, cal.wetAdc);

    zbSoils[i]->setRawAdc(raw);              // expose raw ADC via cluster 0xFC11 attr 0x0005
    zbSoils[i]->setHumidity(moisture);       // moisture % → RH cluster
    zbSoils[i]->setBatteryPercentage(battPct);
    zbSoils[i]->setBatteryVoltage((uint8_t)(battMv / 100));  // genPowerCfg voltage (100 mV units)
    zbSoils[i]->reportHumidity();
    zbSoils[i]->reportRawAdc();              // report raw ADC via cluster 0xFC11 attr 0x0005
  }
  adcPowerOff();
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
  digitalWrite(LED_BUILTIN, HIGH);  // LED off before sleeping
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
}
