// =============================================================================
// zigbee2mqtt external converter – JLT ZB-SoilMeasure  (.mjs / ESM format)
// =============================================================================
//
// INSTALLATION (new z2m ≥ 2.x way – no configuration.yaml changes needed)
// ─────────────────────────────────────────────────────────────────────────────
// Option A – z2m frontend (easiest for HA addon):
//   Settings → Dev console → External converters → paste this file content
//
// Option B – file system:
//   Copy this file to the  external_converters/  subfolder next to
//   configuration.yaml, e.g.:
//     /config/zigbee2mqtt/external_converters/JLT-ZB-SoilMeasure.mjs
//   Files in that folder are loaded automatically on startup.
//
// OTA INDEX (add to configuration.yaml for automatic firmware update discovery)
// ─────────────────────────────────────────────────────────────────────────────
//   ota:
//     zigbee_ota_override_index_location: >-
//       https://raw.githubusercontent.com/JLTSkogshus/JLT-ZB-SoilMeasure/master/ota/index.json
//
// ATTRIBUTES EXPOSED
// ─────────────────────────────────────────────────────────────────────────────
//  soil_moisture_sensor_N  (%)       – moisture reading per sensor (read)
//  cal_dry_sensor_N                  – dry-air ADC value per sensor (read)
//  cal_wet_sensor_N                  – in-water ADC value per sensor (read)
//  sleep_duration          (s)       – deep-sleep interval, device-wide (read/write)
//  firmware_version                  – installed firmware (e.g. "1.0.0") (read)
//  battery                 (%)       – battery level (read)
//  voltage                 (mV)      – battery voltage, 100 mV resolution (read)
//
// NUM_SENSORS  ← must match the value in your firmware config.h
// =============================================================================

import {Zcl}         from 'zigbee-herdsman';
import {battery}     from 'zigbee-herdsman-converters/converters/fromZigbee';
import {presets as e, access as ea} from 'zigbee-herdsman-converters/lib/exposes';
import {deviceAddCustomCluster}     from 'zigbee-herdsman-converters/lib/modernExtend';

const NUM_SENSORS = 2;   // ← change this to match NUM_SENSORS in config.h

// ─── Cluster constants ─────────────────────────────────────────────
const CAL_CLUSTER_CODE = 0xFC11;
const CAL_CLUSTER_NAME = 'jltSoilCal';

// Shared cluster definition for deviceAddCustomCluster().  zigbee-herdsman and
// the z2m frontend require a `name` on the cluster AND on every attribute;
// omitting them makes the frontend's ClusterSinglePicker crash on
// `null.localeCompare(...)` when sorting cluster names.
const CAL_CLUSTER_DEF = {
    name: CAL_CLUSTER_NAME,
    ID: CAL_CLUSTER_CODE,
    attributes: {
        calDry:        {name: 'calDry',        ID: 0x0001, type: Zcl.DataType.UINT16},
        calWet:        {name: 'calWet',        ID: 0x0002, type: Zcl.DataType.UINT16},
        sleepDuration: {name: 'sleepDuration', ID: 0x0003, type: Zcl.DataType.UINT32},
        sleepEnabled:  {name: 'sleepEnabled',  ID: 0x0004, type: Zcl.DataType.UINT8},
        rawAdc:        {name: 'rawAdc',        ID: 0x0005, type: Zcl.DataType.UINT16},
        reportNow:     {name: 'reportNow',     ID: 0x0006, type: Zcl.DataType.UINT8},
    },
    commands:         {},
    commandsResponse: {},
};

// ─── fromZigbee: Relative Humidity cluster → soil_moisture_sensor_N ──────────
const fzMoisture = {
    cluster: 'msRelativeHumidity',
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        const pct = parseFloat(msg.data['measuredValue']) / 100.0;
        // Fire-and-forget read of raw_adc from the calibration cluster on the same
        // endpoint.  The readResponse is handled by fzCal and updates raw_adc_sensor_N.
        // This avoids needing a separate ZCL binding for the custom cluster.
        msg.endpoint.read(CAL_CLUSTER_CODE, [0x0005]).catch(() => {});
        // Battery voltage (genPowerCfg) is not a reportable attribute, so refresh
        // it from endpoint 1 each cycle.  The readResponse is handled by the
        // standard `battery` converter and updates the `voltage` expose.
        if (msg.endpoint.ID === 1) {
            msg.endpoint.read('genPowerCfg', ['batteryVoltage']).catch(() => {});
        }
        return {[`soil_moisture_sensor_${msg.endpoint.ID}`]: pct};
    },
};

// ─── fromZigbee: OTA cluster → human-readable firmware_version ───────────────
// currentFileVersion encoding: 0xMMmmppbb → "MM.mm.pp"
// The device sends its version inside the ZCL "Query Next Image Request" command
// (triggered by requestOTAUpdate()), not as an attribute report.  We catch both
// the command and plain attribute reads so the expose populates either way.
const fzFirmwareVersion = {
    cluster: 'genOta',
    type: ['attributeReport', 'readResponse', 'commandQueryNextImageRequest'],
    convert(model, msg, publish, options, meta) {
        // The Query Next Image Request command carries the running version in
        // `fileVersion`; an attribute read/report carries it in `currentFileVersion`.
        const raw = msg.data['currentFileVersion'] ?? msg.data['fileVersion'];
        if (raw !== undefined) {
            const v     = raw >>> 0;
            const major = (v >>> 24) & 0xFF;
            const minor = (v >>> 16) & 0xFF;
            const patch = (v >>>  8) & 0xFF;
            return {firmware_version: `${major}.${minor}.${patch}`};
        }
    },
};

// ─── fromZigbee: custom calibration cluster ───────────────────────────────────
const fzCal = {
    cluster: CAL_CLUSTER_NAME,
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        const ep     = msg.endpoint.ID;
        const result = {};
        const dry    = msg.data['calDry']        ?? msg.data[0x0001];
        const wet    = msg.data['calWet']        ?? msg.data[0x0002];
        if (dry !== undefined) result[`cal_dry_sensor_${ep}`] = dry;
        if (wet !== undefined) result[`cal_wet_sensor_${ep}`] = wet;
        const sleepEn = msg.data['sleepEnabled'] ?? msg.data[0x0004];
        if (sleepEn !== undefined) result['sleep_enabled'] = sleepEn ? 'ON' : 'OFF';
        const sleepDur = msg.data['sleepDuration'] ?? msg.data[0x0003];
        if (sleepDur !== undefined) result['checkin_interval'] = Math.max(60, sleepDur);
        const rawAdc = msg.data['rawAdc'] ?? msg.data[0x0005];
        if (rawAdc !== undefined) result[`raw_adc_sensor_${ep}`] = rawAdc;
        return result;
    },
};

// ─── fromZigbee: OTA cluster → human-readable firmware_version ───────────────
const tzCal = {
    key: [
        'checkin_interval',
        'sleep_enabled',
        'calibration_target',
        'capture_dry',
        'capture_wet',
        'report_now',
        'cal_dry',
        'cal_wet',
    ],
    convertSet: async (entity, key, value, meta) => {
        // ── Calibration target (dropdown) – state only, no device write ─────────
        if (key === 'calibration_target') {
            return {state: {calibration_target: value}};
        }
        // ── Report now ───────────────────────────────────────────────────────────
        if (key === 'report_now') {
            if (value !== 'ON' && value !== true) return {state: {report_now: 'OFF'}};
            const ep = meta.device.getEndpoint(1);
            // Re-push sleep settings while device is awake so any change made in
            // z2m while the device was asleep takes effect immediately.
            if (meta.state?.sleep_enabled !== undefined) {
                const en = (meta.state.sleep_enabled === 'ON' || meta.state.sleep_enabled === true) ? 1 : 0;
                await ep.write(CAL_CLUSTER_CODE, {0x0004: {value: en, type: Zcl.DataType.UINT8}}).catch(() => {});
            }
            if (meta.state?.checkin_interval !== undefined) {
                const secs = Math.max(60, Math.round(meta.state.checkin_interval));
                await ep.write(CAL_CLUSTER_CODE, {0x0003: {value: secs, type: Zcl.DataType.UINT32}}).catch(() => {});
            }
            await ep.write(CAL_CLUSTER_CODE, {0x0006: {value: 1, type: Zcl.DataType.UINT8}});
            return {state: {report_now: 'OFF'}};
        }
        // ── Capture dry / wet calibration point ──────────────────────────────────
        // Reads the current raw ADC from attr 0x0005 of the selected endpoint,
        // then writes it as the dry (0x0001) or wet (0x0002) calibration value.
        // The device must be awake (sleep_enabled = OFF) for the read to succeed.
        if (key === 'capture_dry' || key === 'capture_wet') {
            if (value !== 'ON' && value !== true) return {state: {[key]: 'OFF'}};
            const target = meta.state.calibration_target || 'sensor_1';
            const epId   = parseInt(target.replace('sensor_', ''), 10);
            if (isNaN(epId) || epId < 1 || epId > NUM_SENSORS)
                throw new Error(`Invalid calibration_target: ${target}`);
            const ep = meta.device.getEndpoint(epId);
            // Use numeric IDs to bypass the cluster-list guard (same pattern as sleep_duration).
            const res    = await ep.read(CAL_CLUSTER_CODE, [0x0005]);
            const rawAdc = res[0x0005] ?? res['rawAdc'];
            if (rawAdc === undefined)
                throw new Error('Could not read rawAdc – ensure the device is awake (sleep_enabled = OFF).');
            const isDry  = key === 'capture_dry';
            await ep.write(CAL_CLUSTER_CODE, {
                [isDry ? 0x0001 : 0x0002]: {value: rawAdc, type: Zcl.DataType.UINT16},
            });
            const calKey = `cal_${isDry ? 'dry' : 'wet'}_sensor_${epId}`;
            return {state: {[key]: 'OFF', [calKey]: rawAdc}};
        }
        // sleep_duration and sleep_enabled are device-wide (no withEndpoint) so
        // entity may be the Device object rather than an Endpoint, which would
        // cause UNSUPPORTED_CLUSTER. Always route them to endpoint 1 explicitly.
        if (key === 'checkin_interval') {
            const secs = Math.max(60, parseInt(value, 10));
            await meta.device.getEndpoint(1).write(CAL_CLUSTER_CODE, {0x0003: {value: secs, type: Zcl.DataType.UINT32}});
            return {state: {checkin_interval: secs}};
        }
        if (key === 'sleep_enabled') {
            const en = (value === 'ON' || value === true || value === 1) ? 1 : 0;
            await meta.device.getEndpoint(1).write(CAL_CLUSTER_CODE, {0x0004: {value: en, type: Zcl.DataType.UINT8}});
            return {state: {sleep_enabled: en ? 'ON' : 'OFF'}};
        }
    },
    convertGet: async (entity, key, meta) => {
        if (key === 'checkin_interval') {
            await meta.device.getEndpoint(1).read(CAL_CLUSTER_CODE, [0x0003]);
        } else if (key === 'sleep_enabled') {
            await meta.device.getEndpoint(1).read(CAL_CLUSTER_CODE, [0x0004]);
        } else if (key === 'cal_dry' || key === 'cal_wet') {
            const epName = meta.endpoint_name || 'sensor_1';
            const epId   = parseInt(epName.replace('sensor_', ''), 10) || 1;
            const attrId = key === 'cal_dry' ? 0x0001 : 0x0002;
            await meta.device.getEndpoint(epId).read(CAL_CLUSTER_CODE, [attrId]);
        }
    },
};

// ─── fromZigbee: genOnOff → user_led (endpoint 4) ────────────────────────────
const fzLed = {
    cluster: 'genOnOff',
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        if (msg.endpoint.ID === 4) {
            return {user_led: msg.data['onOff'] ? 'ON' : 'OFF'};
        }
    },
};

// ─── toZigbee: user_led → genOnOff on/off command to endpoint 4 ──────────────
const tzLed = {
    key: ['user_led'],
    convertSet: async (entity, key, value, meta) => {
        const on = (value === 'ON' || value === true);
        await meta.device.getEndpoint(4).command('genOnOff', on ? 'on' : 'off', {});
        return {state: {user_led: on ? 'ON' : 'OFF'}};
    },
    convertGet: async (entity, key, meta) => {
        await meta.device.getEndpoint(4).read('genOnOff', ['onOff']);
    },
};

// ─── exposes ──────────────────────────────────────────────────────────────────
function buildExposes() {
    const list = [
        e.battery(),
        e.numeric('voltage', ea.STATE)
            .withUnit('mV')
            .withDescription('Battery voltage (100 mV resolution).'),
        e.text('firmware_version', ea.STATE)
            .withDescription('Installed firmware version (e.g. 1.0.0). Updated when device checks for OTA.'),
        e.numeric('checkin_interval', ea.ALL)
            .withUnit('s')
            .withDescription('Deep-sleep duration between reporting cycles (seconds, minimum 60). Takes effect on next wake-up. Enable sleep with sleep_enabled = ON.'),
        e.binary('sleep_enabled', ea.ALL, 'ON', 'OFF')
            .withDescription('Enable deep-sleep between readings. OFF = stay awake (development mode, default). ON = sleep between readings.'),
        e.binary('user_led', ea.ALL, 'ON', 'OFF')
            .withDescription('Onboard user LED (GPIO15, active low).'),
        // ── Calibration UI ───────────────────────────────────────────────────────
        e.enum('calibration_target', ea.STATE_SET,
               Array.from({length: NUM_SENSORS}, (_, i) => `sensor_${i + 1}`))
            .withDescription('Select which sensor to calibrate, then use the Capture buttons below.'),
        e.binary('capture_dry', ea.SET, 'ON', 'OFF')
            .withDescription('Place the selected sensor in dry air, then press to capture the dry calibration point (0 %). Device must be awake.'),
        e.binary('capture_wet', ea.SET, 'ON', 'OFF')
            .withDescription('Place the selected sensor in water, then press to capture the wet calibration point (100 %). Device must be awake.'),
        e.binary('report_now', ea.SET, 'ON', 'OFF')
            .withDescription('Trigger an immediate sensor report without waiting for the next scheduled interval. Device must be awake (sleep_enabled = OFF).'),
    ];
    for (let i = 1; i <= NUM_SENSORS; i++) {
        list.push(
            e.numeric('soil_moisture', ea.STATE)
                .withUnit('%')
                .withDescription(`Sensor ${i} soil moisture`)
                .withEndpoint(`sensor_${i}`),
            e.numeric('raw_adc', ea.STATE)
                .withDescription(`Sensor ${i} last raw ADC reading (0–4095). Updated each reporting cycle.`)
                .withEndpoint(`sensor_${i}`),
            e.numeric('cal_dry', ea.STATE_GET)
                .withDescription(`Sensor ${i}: ADC value in dry air (→ 0 %). Set via Capture dry above.`)
                .withEndpoint(`sensor_${i}`),
            e.numeric('cal_wet', ea.STATE_GET)
                .withDescription(`Sensor ${i}: ADC value submerged in water (→ 100 %). Set via Capture wet above.`)
                .withEndpoint(`sensor_${i}`),
        );
    }
    return list;
}

function endpointMap() {
    const map = {user_led: 4};
    for (let i = 1; i <= NUM_SENSORS; i++) map[`sensor_${i}`] = i;
    return map;
}

// ─── Device definition ────────────────────────────────────────────────────────
/** @type{import('zigbee-herdsman-converters/lib/types').DefinitionWithExtend} */
export default {
    zigbeeModel: ['ZB-SoilMeasure'],
    model:       'ZB-SoilMeasure',
    vendor:      'JLT',
    description: 'Zigbee soil moisture sensor – 1 to 9 probes, ADS1115 + onboard ADC, battery powered, deep-sleep capable.',
    fromZigbee:  [fzMoisture, battery, fzCal, fzFirmwareVersion, fzLed],
    toZigbee:    [tzCal, tzLed],
    exposes:     buildExposes(),
    ota:         true,
    meta:        {multiEndpoint: true},
    endpoint:    () => endpointMap(),
    // Register the manufacturer-specific calibration cluster ON THE DEVICE so
    // zigbee-herdsman resolves 0xFC11 → 'jltSoilCal' for this device.  Without
    // this, incoming frames stay as the raw numeric cluster 64529 and fzCal
    // (which matches cluster: 'jltSoilCal') never fires → no raw_adc / cal values.
    extend:      [deviceAddCustomCluster(CAL_CLUSTER_NAME, CAL_CLUSTER_DEF)],
    // Re-push sleep_enabled and checkin_interval to the device on every wakeup
    // so that changes made in z2m while the device was sleeping take effect on
    // the next wakeup period (the write is silently lost when the device is asleep;
    // z2m does not automatically retry it).
    onEvent: async (type, data, device, settings, state) => {
        if (type !== 'deviceAnnounce') return;
        const ep = device.getEndpoint(1);
        if (!ep) return;
        if (state?.sleep_enabled !== undefined) {
            const en = (state.sleep_enabled === 'ON' || state.sleep_enabled === true) ? 1 : 0;
            ep.write(CAL_CLUSTER_CODE, {0x0004: {value: en, type: Zcl.DataType.UINT8}}).catch(() => {});
        }
        if (state?.checkin_interval !== undefined) {
            const secs = Math.max(60, Math.round(state.checkin_interval));
            ep.write(CAL_CLUSTER_CODE, {0x0003: {value: secs, type: Zcl.DataType.UINT32}}).catch(() => {});
        }
    },
    // Bind coordinator to each sensor endpoint so z2m routes incoming attribute
    // reports through the fromZigbee converters.  The device must be awake.
    configure: async (device, coordinatorEndpoint) => {
        for (let i = 1; i <= NUM_SENSORS; i++) {
            const ep = device.getEndpoint(i);
            if (!ep) continue;
            try { await ep.bind('msRelativeHumidity', coordinatorEndpoint); } catch (_) {}
            try { await ep.bind('genPowerCfg',        coordinatorEndpoint); } catch (_) {}
            // Bind the custom calibration cluster too.  The firmware now reports
            // its 0xFC11 attributes via DIRECT addressing to the coordinator
            // (so a binding is not strictly required), but binding is harmless
            // and keeps the binding table consistent.
            try { await ep.bind(CAL_CLUSTER_CODE, coordinatorEndpoint); } catch (_) {}
            // For sensor 1 also read device-wide attrs (sleep_duration 0x0003, sleep_enabled 0x0004)
            // in the same request to avoid a separate round-trip that can time out.
            const calAttrs = i === 1
                ? [0x0001, 0x0002, 0x0003, 0x0004, 0x0005]
                : [0x0001, 0x0002, 0x0005];
            try { await ep.read(CAL_CLUSTER_CODE, calAttrs); } catch (_) {}
        }
        // Read firmware version from the OTA upgrade cluster.
        try { await device.getEndpoint(1).read('genOta', ['currentFileVersion']); } catch (_) {}
        // Read battery percentage + voltage (genPowerCfg) so battery / voltage populate.
        try { await device.getEndpoint(1).read('genPowerCfg', ['batteryPercentageRemaining', 'batteryVoltage']); } catch (_) {}
        // Read the user LED on/off state (endpoint 4) so user_led populates.
        try { await device.getEndpoint(4).read('genOnOff', ['onOff']); } catch (_) {}
    },
};
