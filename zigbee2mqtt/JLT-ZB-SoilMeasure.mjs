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
//  cal_dry_sensor_N                  – dry-air ADC value per sensor (read/write)
//  cal_wet_sensor_N                  – in-water ADC value per sensor (read/write)
//  sleep_duration          (s)       – deep-sleep interval, device-wide (read/write)
//  firmware_version                  – installed firmware (e.g. "1.0.0") (read)
//  battery                 (%)       – battery level (read)
//
// NUM_SENSORS  ← must match the value in your firmware config.h
// =============================================================================

import {Zcl}         from 'zigbee-herdsman';
import {battery}     from 'zigbee-herdsman-converters/converters/fromZigbee';
import {presets as e, access as ea} from 'zigbee-herdsman-converters/lib/exposes';

const NUM_SENSORS = 3;   // ← change this to match NUM_SENSORS in config.h

// ─── Cluster constants ────────────────────────────────────────────────────────
const CAL_CLUSTER_CODE = 0xFC11;
const CAL_CLUSTER_NAME = 'jltSoilCal';

// ─── Register the manufacturer-specific cluster with zigbee-herdsman ─────────
try {
    const addFn = (Zcl.Utils && Zcl.Utils.addCluster)
                ? (def) => Zcl.Utils.addCluster(def)
                : (def) => Zcl.addCluster(def);
    addFn({
        name: CAL_CLUSTER_NAME,
        code: CAL_CLUSTER_CODE,
        attributes: {
            calDry:        {ID: 0x0001, type: Zcl.DataType.UINT16},
            calWet:        {ID: 0x0002, type: Zcl.DataType.UINT16},
            sleepDuration: {ID: 0x0003, type: Zcl.DataType.UINT32},
            sleepEnabled:  {ID: 0x0004, type: Zcl.DataType.UINT8},
        },
        commands:         {},
        commandsResponse: {},
    });
} catch (_) {
    // Cluster already registered (e.g. hot-reload) – patch in any missing attributes.
    try {
        const findFn = Zcl.Utils ? Zcl.Utils.findCluster : Zcl.findCluster;
        const existing = findFn && findFn(CAL_CLUSTER_NAME);
        if (existing && !existing.attributes.sleepEnabled) {
            existing.attributes.sleepEnabled = {ID: 0x0004, type: Zcl.DataType.UINT8};
        }
    } catch (_2) { /* best-effort */ }
}

// ─── fromZigbee: Relative Humidity cluster → soil_moisture_sensor_N ──────────
const fzMoisture = {
    cluster: 'msRelativeHumidity',
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        const pct = parseFloat(msg.data['measuredValue']) / 100.0;
        return {[`soil_moisture_sensor_${msg.endpoint.ID}`]: pct};
    },
};

// ─── fromZigbee: OTA cluster → human-readable firmware_version ───────────────
// currentFileVersion encoding: 0xMMmmppbb → "MM.mm.pp"
const fzFirmwareVersion = {
    cluster: 'genOta',
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        if (msg.data['currentFileVersion'] !== undefined) {
            const v     = msg.data['currentFileVersion'] >>> 0;
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
        const sleep  = msg.data['sleepDuration'] ?? msg.data[0x0003];
        if (dry   !== undefined) result[`cal_dry_sensor_${ep}`] = dry;
        if (wet   !== undefined) result[`cal_wet_sensor_${ep}`] = wet;
        if (sleep !== undefined) result['sleep_duration']       = sleep;
        const sleepEn = msg.data['sleepEnabled'] ?? msg.data[0x0004];
        if (sleepEn !== undefined) result['sleep_enabled'] = sleepEn ? 'ON' : 'OFF';
        return result;
    },
};

// ─── toZigbee: write calibration / sleep duration ────────────────────────────
const tzCal = {
    key: [
        'sleep_duration',
        'sleep_enabled',
        ...Array.from({length: NUM_SENSORS}, (_, i) => `cal_dry_sensor_${i + 1}`),
        ...Array.from({length: NUM_SENSORS}, (_, i) => `cal_wet_sensor_${i + 1}`),
    ],
    convertSet: async (entity, key, value, meta) => {
        // sleep_duration and sleep_enabled are device-wide (no withEndpoint) so
        // entity may be the Device object rather than an Endpoint, which would
        // cause UNSUPPORTED_CLUSTER. Always route them to endpoint 1 explicitly.
        if (key === 'sleep_duration') {
            const secs = Math.max(60, parseInt(value, 10));
            // Use numeric cluster/attr IDs to bypass z2m-herdsman's endpoint
            // cluster-list guard (avoids UNSUPPORTED_CLUSTER if interview data
            // is stale / incomplete).
            await meta.device.getEndpoint(1).write(CAL_CLUSTER_CODE, {0x0003: {value: secs, type: Zcl.DataType.UINT32}});
            return {state: {sleep_duration: secs}};
        }
        if (key === 'sleep_enabled') {
            const en = (value === 'ON' || value === true || value === 1) ? 1 : 0;
            await meta.device.getEndpoint(1).write(CAL_CLUSTER_CODE, {0x0004: {value: en, type: Zcl.DataType.UINT8}});
            return {state: {sleep_enabled: en ? 'ON' : 'OFF'}};
        }
        const m = key.match(/^cal_(dry|wet)_sensor_(\d+)$/);
        if (m) {
            const attr = m[1] === 'dry' ? 'calDry' : 'calWet';
            await entity.write(CAL_CLUSTER_NAME, {[attr]: parseInt(value, 10)});
            return {state: {[key]: parseInt(value, 10)}};
        }
    },
    convertGet: async (entity, key, meta) => {
        if (key === 'sleep_duration') {
            // Numeric IDs bypass the cluster-list guard in z2m-herdsman.
            await meta.device.getEndpoint(1).read(CAL_CLUSTER_CODE, [0x0003]);
        } else if (key === 'sleep_enabled') {
            await meta.device.getEndpoint(1).read(CAL_CLUSTER_CODE, [0x0004]);
        } else {
            const m    = key.match(/^cal_(dry|wet)_sensor_(\d+)$/);
            const attr = m ? (m[1] === 'dry' ? 'calDry' : 'calWet') : null;
            if (attr) await entity.read(CAL_CLUSTER_NAME, [attr]);
        }
    },
};

// ─── exposes ──────────────────────────────────────────────────────────────────
function buildExposes() {
    const list = [
        e.battery(),
        e.text('firmware_version', ea.STATE)
            .withDescription('Installed firmware version (e.g. 1.0.0). Updated when device checks for OTA.'),
        e.numeric('sleep_duration', ea.ALL)
            .withUnit('s')
            .withDescription('Deep-sleep interval in seconds (writable). Minimum 60 s. Takes effect on next wake-up.'),
        e.binary('sleep_enabled', ea.ALL, 'ON', 'OFF')
            .withDescription('Enable deep-sleep between readings. OFF = stay awake (development mode, default). ON = sleep between readings.'),
    ];
    for (let i = 1; i <= NUM_SENSORS; i++) {
        list.push(
            e.numeric('soil_moisture', ea.STATE)
                .withUnit('%')
                .withDescription(`Sensor ${i} soil moisture`)
                .withEndpoint(`sensor_${i}`),
            e.numeric('cal_dry', ea.ALL)
                .withDescription(`Sensor ${i}: ADC value in dry air (→ 0 %). Place sensor in air and write the raw ADC shown on serial.`)
                .withEndpoint(`sensor_${i}`),
            e.numeric('cal_wet', ea.ALL)
                .withDescription(`Sensor ${i}: ADC value submerged in water (→ 100 %). Place sensor in water and write the raw ADC shown on serial.`)
                .withEndpoint(`sensor_${i}`),
        );
    }
    return list;
}

function endpointMap() {
    const map = {};
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
    fromZigbee:  [fzMoisture, battery, fzCal, fzFirmwareVersion],
    toZigbee:    [tzCal],
    exposes:     buildExposes(),
    ota:         true,
    meta:        {multiEndpoint: true},
    endpoint:    () => endpointMap(),
    // Sleepy end device – pushes data on wake-up, no cluster binds needed.
    configure:   async () => {},
};
