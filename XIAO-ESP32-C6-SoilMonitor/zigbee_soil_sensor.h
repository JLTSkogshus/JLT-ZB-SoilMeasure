#pragma once
#include <ep/ZigbeeTempSensor.h>
#include "calibration.h"

// =============================================================================
// ZigbeeSoilSensor – ZigbeeTempSensor subclass for soil moisture monitoring
// =============================================================================
// Clusters exposed on each endpoint:
//   0x0000  Basic                – manufacturer / model
//   0x0001  Power Configuration  – battery %
//   0x0402  Temperature          – unused (reports 0); present because base class
//   0x0405  Relative Humidity    – soil moisture %  (zigbee2mqtt maps → humidity)
//   0xFC11  Calibration (R/W)    – attr 0x0001 dry ADC, attr 0x0002 wet ADC
//
// Calibration attributes are writable from zigbee2mqtt dev console and are
// persisted to NVS immediately on write.
// =============================================================================

class ZigbeeSoilSensor : public ZigbeeTempSensor {
public:
    ZigbeeSoilSensor(uint8_t endpoint, uint8_t sensorIdx);
    ~ZigbeeSoilSensor() {}

    // Sets the Basic cluster SW Build ID string (0x4000), shown as
    // "Firmware version" in the zigbee2mqtt About tab.
    // Must be called before Zigbee.begin().
    void setSoftwareBuildId(const char* id);

protected:
    // Called by the Zigbee stack when the coordinator writes an attribute.
    // Handles writes to cluster 0xFC11 (dry / wet calibration).
    void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override;

private:
    uint8_t _sensorIdx;
    void    _addCalibrationCluster();
};
