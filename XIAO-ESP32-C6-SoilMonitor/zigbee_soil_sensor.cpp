#include "zigbee_soil_sensor.h"
#include "calibration.h"
#include "config.h"

// Static per-sensor attribute storage – pointers passed to ZCL must remain valid
// for the lifetime of the endpoint (i.e. forever for global objects).
static uint16_t s_calDry[10];
static uint16_t s_calWet[10];
static uint16_t s_rawAdc[10]     = {};  // last raw ADC reading per sensor (read-only attribute)
static uint8_t  s_sleepEnabled;        // device-wide: 0 = awake mode, 1 = deep-sleep mode
static uint8_t  s_reportNowAttr = 0;   // ZCL backing store for report_now attribute
static volatile bool s_reportNow = false; // set by Zigbee task, read by app task

// ── genPollCtrl (0x0020) staging store – quarter-second units ───────────────
// Holds the desired check_in_interval value from NVS so applyCheckInInterval()
// can push it into the ZCL attribute after Zigbee.begin().
static uint32_t s_checkInIntervalQs;   // seeded from NVS in updateCalFromNvs()

// =============================================================================
// Constructor
// =============================================================================
ZigbeeSoilSensor::ZigbeeSoilSensor(uint8_t endpoint, uint8_t sensorIdx)
    : ZigbeeTempSensor(endpoint), _sensorIdx(sensorIdx) {
    // Parent constructor has already built _cluster_list (protected member).
    // Calibration is lazy-init safe – NVS will be opened on first access.
    addHumiditySensor();       // add Relative Humidity cluster (0x0405)
    _addCalibrationCluster();  // add writable calibration cluster (0xFC11)
    if (_sensorIdx == 0) {
        _addPollControlCluster();  // genPollCtrl (0x0020) – device-wide sleep interval
    }
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
    if (_sensorIdx == 0) {
        s_sleepEnabled = Calibration.getSleepEnabled() ? 1u : 0u;
    }

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
        attrs, CAL_ATTR_SLEEP_ENABLE,
        ESP_ZB_ZCL_ATTR_TYPE_U8,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        &s_sleepEnabled);

    esp_zb_custom_cluster_add_custom_attr(
        attrs, CAL_ATTR_RAW_ADC,
        ESP_ZB_ZCL_ATTR_TYPE_U16,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,   // READ_ONLY|REPORTING (0x05) not valid for custom cluster attrs
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
// _addPollControlCluster()
// Adds genPollCtrl (0x0020) to endpoint 1 using the NATIVE cluster creation
// API.  This ensures the cluster appears in the Simple Descriptor so that
// zigbee2mqtt (via zigbee-herdsman) can verify the device supports it before
// sending ZCL Write Attributes – avoiding the UNSUPPORTED_CLUSTER rejection
// that occurs when the cluster is registered via esp_zb_cluster_list_add_custom_cluster.
//
// Actual check_in_interval value is pushed into the ZCL store after
// Zigbee.begin() + connected via applyCheckInInterval().
// =============================================================================
void ZigbeeSoilSensor::_addPollControlCluster() {
    s_checkInIntervalQs = (uint32_t)SLEEP_DURATION_SEC * 4u;  // staging; overwritten by updateCalFromNvs()
    esp_zb_poll_control_cluster_cfg_t cfg = {};
    esp_zb_attribute_list_t *pc = esp_zb_poll_control_cluster_create(&cfg);
    esp_zb_cluster_list_add_poll_control_cluster(_cluster_list, pc, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

// =============================================================================
// applyCheckInInterval()
// Writes s_checkInIntervalQs (set by updateCalFromNvs()) into the ZCL
// attribute store for the native genPollCtrl cluster.  Must be called after
// Zigbee.begin() since esp_zb_zcl_set_attribute_val() needs the stack running.
// =============================================================================
void ZigbeeSoilSensor::applyCheckInInterval() {
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(
        _endpoint, 0x0020U, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        0x0000U, &s_checkInIntervalQs, false);
    esp_zb_lock_release();
}

// =============================================================================
// zbAttributeSet()
// Called by the Zigbee stack whenever the coordinator writes an attribute on
// this endpoint.  Handles cluster 0xFC11 (calibration) and cluster 0x0020
// (genPollCtrl – check_in_interval → sleep duration).
// =============================================================================
void ZigbeeSoilSensor::zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) {
    if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return;

    // ── genPollCtrl (0x0020) – device-wide sleep interval ────────────────────
    if (message->info.cluster == 0x0020U) {
        if (message->attribute.id == 0x0000U) {  // check_in_interval
            uint32_t qs   = *reinterpret_cast<const uint32_t *>(message->attribute.data.value);
            uint32_t secs = (qs / 4u < 60u) ? 60u : (qs / 4u);
            s_checkInIntervalQs = secs * 4u;
            Calibration.setSleepSeconds(secs);
            Serial.printf("[pollCtrl] check_in_interval → %lu s\n", (unsigned long)secs);
        }
        return;
    }

    // ── jltSoilCal (0xFC11) ──────────────────────────────────────────────────
    if (message->info.cluster != CAL_CLUSTER_ID) return;

    SensorCalibration cal = Calibration.get(_sensorIdx);

    switch (message->attribute.id) {
        case CAL_ATTR_DRY:
            cal.dryAdc = *reinterpret_cast<const uint16_t *>(message->attribute.data.value);
            break;
        case CAL_ATTR_WET:
            cal.wetAdc = *reinterpret_cast<const uint16_t *>(message->attribute.data.value);
            break;
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
    // Push the value into the ZCL attribute database.  Writing the static
    // backing variable alone is NOT enough: esp_zb_zcl_report_attr_cmd_req()
    // (and coordinator reads) return the value held in ZBOSS's own attribute
    // storage, which is only seeded from our pointer at registration time
    // (before Zigbee.begin()).  Runtime updates must go through this API or the
    // reported value stays at its initial 0.
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_set_attribute_val(_endpoint, CAL_CLUSTER_ID,
                                 ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                                 CAL_ATTR_RAW_ADC, &s_rawAdc[_sensorIdx], false);
    esp_zb_lock_release();
}

// =============================================================================
// updateCalFromNvs()
// Refreshes all ZCL backing stores from NVS.  Call from setup() after
// Calibration.begin() to guarantee correct values even when NVS was not
// ready during the static constructor (common on first boot after factory flash).
// =============================================================================
void ZigbeeSoilSensor::updateCalFromNvs() {
    SensorCalibration cal = Calibration.get(_sensorIdx);
    s_calDry[_sensorIdx] = cal.dryAdc;
    s_calWet[_sensorIdx] = cal.wetAdc;
    // Device-wide attrs: only update if we own sensor index 0 to avoid
    // overwriting values already set by a sibling sensor instance.
    if (_sensorIdx == 0) {
        // Refresh genPollCtrl backing store so the ZCL attribute reflects NVS.
        s_checkInIntervalQs = (uint32_t)Calibration.getSleepSeconds() * 4u;
        s_sleepEnabled = Calibration.getSleepEnabled() ? 1u : 0u;
    }
}

// =============================================================================
// _reportCustomAttr()
// Sends a single attribute of the calibration cluster (0xFC11) to the
// coordinator using DIRECT addressing (short addr 0x0000, endpoint 1).
//
// Direct addressing is used instead of binding-table mode because z2m does NOT
// auto-create a binding for the manufacturer-specific cluster (only standard
// clusters like Relative Humidity are auto-bound during interview).  A direct
// report reaches the coordinator regardless of the binding table, and z2m
// routes every incoming report through the fromZigbee converters.
// =============================================================================
bool ZigbeeSoilSensor::_reportCustomAttr(uint16_t attrId) {
    esp_zb_zcl_report_attr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.address_mode                        = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000U;   // coordinator
    cmd.zcl_basic_cmd.dst_endpoint          = 1U;        // coordinator endpoint
    cmd.zcl_basic_cmd.src_endpoint          = _endpoint;
    cmd.clusterID                           = CAL_CLUSTER_ID;
    cmd.attributeID                         = attrId;
    cmd.direction                           = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.manuf_specific                      = 0x00U;
    cmd.dis_default_resp                    = 0x00U;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t ret = esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_lock_release();
    if (ret != ESP_OK) {
        log_e("Failed to report attr 0x%04X: 0x%x: %s", attrId, ret, esp_err_to_name(ret));
        return false;
    }
    return true;
}

// =============================================================================
// reportRawAdc()
// Pushes all calibration-cluster values to the coordinator each reporting cycle
// via direct addressing: rawAdc / calDry / calWet for this endpoint, plus the
// device-wide sleep attributes (reported once from sensor index 0 / endpoint 1).
// Must be called AFTER setRawAdc() each reporting cycle.
// =============================================================================
bool ZigbeeSoilSensor::reportRawAdc() {
    bool ok = true;
    ok &= _reportCustomAttr(CAL_ATTR_RAW_ADC);
    ok &= _reportCustomAttr(CAL_ATTR_DRY);
    ok &= _reportCustomAttr(CAL_ATTR_WET);
    // Device-wide attributes – report only from sensor 0 (endpoint 1) to avoid
    // duplicate frames; the values live in shared static storage.
    if (_sensorIdx == 0) {
        ok &= _reportCustomAttr(CAL_ATTR_SLEEP_ENABLE);
    }
    return ok;
}

// =============================================================================
// Free functions for the app task to check / clear the report-now flag.
// The flag is set by the Zigbee task (zbAttributeSet) and consumed by setup().
// =============================================================================
bool zigbeeSoilGetReportNow()   { return s_reportNow; }
void zigbeeSoilClearReportNow() { s_reportNow = false; }
