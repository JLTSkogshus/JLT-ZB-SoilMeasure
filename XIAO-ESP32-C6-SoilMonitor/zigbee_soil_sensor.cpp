#include "zigbee_soil_sensor.h"
#include "calibration.h"
#include "config.h"

// Static per-sensor attribute storage – pointers passed to ZCL must remain valid
// for the lifetime of the endpoint (i.e. forever for global objects).
static uint16_t s_calDry[9];
static uint16_t s_calWet[9];
static uint32_t s_sleepSec;   // device-wide, shared by all endpoint instances

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
    s_sleepSec           = Calibration.getSleepSeconds();

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
        default:
            return;
    }
    Calibration.set(_sensorIdx, cal);
}
