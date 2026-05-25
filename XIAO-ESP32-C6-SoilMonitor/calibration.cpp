#include "calibration.h"

CalibrationManager Calibration;

static constexpr const char *NVS_NS = "soil_cal";

void CalibrationManager::ensureInit() {
    if (!_initialized) {
        _prefs.begin(NVS_NS, false);
        _initialized = true;
    }
}

void CalibrationManager::begin() {
    ensureInit();
    Serial.println("[cal] NVS calibration store opened.");
}

SensorCalibration CalibrationManager::get(uint8_t idx) {
    ensureInit();
    char key[8];
    SensorCalibration cal;
    snprintf(key, sizeof(key), "dry%d", idx);
    cal.dryAdc = _prefs.getUShort(key, CAL_DRY_DEFAULT);
    snprintf(key, sizeof(key), "wet%d", idx);
    cal.wetAdc = _prefs.getUShort(key, CAL_WET_DEFAULT);
    return cal;
}

void CalibrationManager::set(uint8_t idx, const SensorCalibration &cal) {
    ensureInit();
    char key[8];
    snprintf(key, sizeof(key), "dry%d", idx);
    _prefs.putUShort(key, cal.dryAdc);
    snprintf(key, sizeof(key), "wet%d", idx);
    _prefs.putUShort(key, cal.wetAdc);
    Serial.printf("[cal] Sensor %d saved  dry=%u  wet=%u\n", idx + 1, cal.dryAdc, cal.wetAdc);
}

void CalibrationManager::resetToDefaults() {
    ensureInit();
    _prefs.clear();
    Serial.println("[cal] All calibration erased – factory defaults will apply.");
}

uint32_t CalibrationManager::getSleepSeconds() {
    ensureInit();
    return _prefs.getUInt("sleep_sec", SLEEP_DURATION_SEC);
}

void CalibrationManager::setSleepSeconds(uint32_t seconds) {
    ensureInit();
    _prefs.putUInt("sleep_sec", seconds);
    Serial.printf("[cal] Sleep duration updated to %lu s\n", (unsigned long)seconds);
}

bool CalibrationManager::getSleepEnabled() {
    ensureInit();
    return _prefs.getBool("sleep_en", false);  // default: awake mode
}

void CalibrationManager::setSleepEnabled(bool enabled) {
    ensureInit();
    _prefs.putBool("sleep_en", enabled);
    Serial.printf("[cal] Sleep mode %s\n", enabled ? "ENABLED" : "DISABLED");
}
