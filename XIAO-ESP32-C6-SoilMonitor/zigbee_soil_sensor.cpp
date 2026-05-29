#include "zigbee_soil_sensor.h"
#include "calibration.h"
#include "config.h"

// Static per-sensor attribute storage – pointers passed to ZCL must remain valid
// for the lifetime of the endpoint (i.e. forever for global objects).
static uint16_t s_calDry[10];
static uint16_t s_calWet[10];
static uint16_t s_rawAdc[10]     = {};  // last raw ADC reading per sensor (read-only attribute)
static uint32_t s_sleepSec;            // device-wide sleep duration (seconds)
static uint8_t  s_sleepEnabled;        // device-wide: 0 = awake mode, 1 = deep-sleep mode
static uint8_t  s_reportNowAttr = 0;   // ZCL backing store for report_now attribute
static volatile bool s_reportNow = false; // set by Zigbee task, read by app task

// =============================================================================
// Constructor
// =============================================================================
ZigbeeSoilSensor::ZigbeeSoilSensor(uint8_t endpoint, uint8_t sensorIdx)
    : ZigbeeTempSensor(endpoint), _sensorIdx(sensorIdx) {
    // Parent constructor has already built _cluster_list (protected member).
    // Calibration is lazy-init safe – NVS will be opened on first access.
    addHumiditySensor();       // add Relative Humidity cluster (0x0405)
    _addCalibrationCluster();  // add writable calibration cluster (0xFC11)
}

// =============================================================================
// _addCalibrationCluster()
// Appends the manufacturer-specific calibration cluster to _cluster_list.
// Must be called after the parent constructor so _cluster_list is valid.
// =============================================================================
void ZigbeeSoilSensor::_addCalibrationCluster() {
    SensorCalibration cal = Calibration.get(_sensorIdx);

    // Store initial values in static arrays – ZCL holds a pointer to these.
    s_calDry[_sensorIdx] = cal.dryAdc;
    s_calWet[_sensorIdx] = cal.wetAdc;
    s_sleepSec     = Calibration.getSleepSeconds();
    s_sleepEnabled = Calibration.getSleepEnabled() ? 1u : 0u;

    esp_zb_attribute_list_t *attrs = esp_zb_zcl_attr_list_create(CAL_CLUSTER_ID);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_DRY,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &s_calDry[_sensorIdx]);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_WET,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &s_calWet[_sensorIdx]);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_SLEEP,
        ESP_ZB_ZCL_ATTR_TYPE_U32,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &s_sleepSec);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_SLEEP_ENABLE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &s_sleepEnabled);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_RAW_ADC,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY,
        &s_rawAdc[_sensorIdx]);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_REPORT_NOW,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &s_reportNowAttr);

    esp_zb_cluster_list_add_custom_cluster(
        _cluster_list, attrs,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

// =============================================================================
// zbAttributeSet()
// Called by the Zigbee stack whenever the coordinator writes an attribute on
// this endpoint.  Only cluster 0xFC11 (calibration) is handled here.
// =============================================================================
void ZigbeeSoilSensor::zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) {
    if (message->info.status  != ESP_ZB_ZCL_STATUS_SUCCESS) return;
    if (message->info.cluster != CAL_CLUSTER_ID)            return;

    SensorCalibration cal = Calibration.get(_sensorIdx);

    switch (message->attribute.id) {
        case CAL_ATTR_DRY:
            cal.dryAdc = *reinterpret_cast<const uint16_t *>(message->attribute.data.value);
            break;
        case CAL_ATTR_WET:
            cal.wetAdc = *reinterpret_cast<const uint16_t *>(message->attribute.data.value);
            break;
        case CAL_ATTR_SLEEP: {
            uint32_t secs = *reinterpret_cast<const uint32_t *>(message->attribute.data.value);
            if (secs > 0) {
                s_sleepSec = secs;
                Calibration.setSleepSeconds(secs);
            }
            return;   // not a calibration attribute – skip Calibration.set() below
        }
        case CAL_ATTR_SLEEP_ENABLE: {
            uint8_t en = *reinterpret_cast<const uint8_t *>(message->attribute.data.value);
            s_sleepEnabled = en;
            Calibration.setSleepEnabled(en != 0);
            return;
        }
        case CAL_ATTR_REPORT_NOW: {
            uint8_t val = *reinterpret_cast<const uint8_t *>(message->attribute.data.value);
            if (val) s_reportNow = true;
            return;
        }
        default:
            return;
    }
    Calibration.set(_sensorIdx, cal);
}

// =============================================================================
// setRawAdc()
// Stores the latest raw ADC reading into the ZCL attribute table so the
// coordinator can read it on demand (e.g. when user presses Capture Dry/Wet).
// =============================================================================
void ZigbeeSoilSensor::setRawAdc(uint16_t raw) {
    s_rawAdc[_sensorIdx] = raw;
}

// =============================================================================
// Free functions for the app task to check / clear the report-now flag.
// The flag is set by the Zigbee task (zbAttributeSet) and consumed by setup().
// =============================================================================
bool zigbeeSoilGetReportNow()   { return s_reportNow; }
void zigbeeSoilClearReportNow() { s_reportNow = false; }
