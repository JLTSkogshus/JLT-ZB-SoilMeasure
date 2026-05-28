'use strict';

// =============================================================================
// zigbee2mqtt external converter – JLT ZB-SoilMeasure
// =============================================================================
//
// INSTALLATION
// ─────────────────────────────────────────────────────────────────────────────
// 1. Copy this file to your zigbee2mqtt data directory, e.g.:
//      /opt/zigbee2mqtt/data/JLT-ZB-SoilMeasure.js    (Linux/Docker)
//      C:\zigbee2mqtt\data\JLT-ZB-SoilMeasure.js       (Windows)
//
// 2. Add to configuration.yaml:
//      external_converters:
//        - JLT-ZB-SoilMeasure.js
//
//      ota:
//        zigbee_ota_override_index_location: 'https://raw.githubusercontent.com/JLTSkogshus/JLT-ZB-SoilMeasure/master/ota/index.json'
//
//    This lets zigbee2mqtt automatically discover the latest firmware from
//    GitHub Releases.  When a new version is published the OTA tab in the
//    device page will offer the update; no manual file copying needed.
//
// 3. Restart zigbee2mqtt.
//
// ATTRIBUTES EXPOSED
// ─────────────────────────────────────────────────────────────────────────────
//  soil_moisture_sensor_N  (%)         – moisture reading per sensor (read)
//  cal_dry_sensor_N                    – dry-air ADC value per sensor (read/write)
//  cal_wet_sensor_N                    – in-water ADC value per sensor (read/write)
//  sleep_duration          (seconds)   – deep-sleep interval, device-wide (read/write)
//  battery                 (%)         – battery level (read)
//
//  "Last connection" / "Last seen" is shown in the z2m device list and header –
//  no firmware attribute needed for that.
//
//  "Next wakeup" can be calculated in Home Assistant as a template sensor:
//    {{ (as_timestamp(states['sensor.<device>_last_seen']) +
//        states('sensor.<device>_sleep_duration') | int) | timestamp_local }}
//
// NUM_SENSORS  ← must match the value in your firmware config.h
// =============================================================================

const NUM_SENSORS = 3;   // ← change this to match NUM_SENSORS in config.h

// ─── Cluster constants ────────────────────────────────────────────────────────
const CAL_CLUSTER_CODE = 0xFC11;
const CAL_CLUSTER_NAME = 'jltSoilCal';

// ─── Register the manufacturer-specific cluster with zigbee-herdsman ─────────
// This lets the library parse attribute reports by name instead of raw ID.
(function registerCluster() {
    try {
        const {Zcl} = require('zigbee-herdsman');
        // zigbee-herdsman ≥ 3.x uses Zcl.Utils.addCluster
        const addFn = (Zcl.Utils && Zcl.Utils.addCluster)
                    ? (def) => Zcl.Utils.addCluster(def)
                    : (def) => Zcl.addCluster(def);   // fallback for older herdsman
        addFn({
            name: CAL_CLUSTER_NAME,
            code: CAL_CLUSTER_CODE,
            attributes: {
                calDry:        {ID: 0x0001, type: Zcl.DataType.UINT16},
                calWet:        {ID: 0x0002, type: Zcl.DataType.UINT16},
                sleepDuration: {ID: 0x0003, type: Zcl.DataType.UINT32},
                rawAdc:        {ID: 0x0005, type: Zcl.DataType.UINT16},
            },
            commands:         {},
            commandsResponse: {},
        });
    } catch (_) {
        // Cluster already registered (e.g. hot-reload during development)
    }
}());

// ─── fromZigbee: Relative Humidity cluster → soil_moisture_sensor_N ──────────
const fzMoisture = {
    cluster: 'msRelativeHumidity',
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        const pct = parseFloat(msg.data['measuredValue']) / 100.0;
        return {[`soil_moisture_sensor_${msg.endpoint.ID}`]: pct};
    },
};

// ─── fromZigbee: OTA cluster → human-readable firmware_version ──────────────
// Fires when z2m reads genOta attributes (on join and when checking for updates).
// currentFileVersion encoding: 0xMMmmppbb → "MM.mm.pp"
const fzFirmwareVersion = {
    cluster: 'genOta',
    type: ['attributeReport', 'readResponse'],
    convert(model, msg, publish, options, meta) {
        if (msg.data['currentFileVersion'] !== undefined) {
            const v     = msg.data['currentFileVersion'] >>> 0;   // force uint32
            const major = (v >>> 24) & 0xFF;
            const minor = (v >>> 16) & 0xFF;
            const patch = (v >>>  8) & 0xFF;
            return { firmware_version: `${major}.${minor}.${patch}` };
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
        // Attributes arrive by name when cluster is registered, else by raw ID.
        const dry   = msg.data['calDry']        ?? msg.data[0x0001];
        const wet   = msg.data['calWet']        ?? msg.data[0x0002];
        const sleep = msg.data['sleepDuration'] ?? msg.data[0x0003];
        if (dry   !== undefined) result[`cal_dry_sensor_${ep}`]  = dry;
        if (wet   !== undefined) result[`cal_wet_sensor_${ep}`]  = wet;
        if (sleep !== undefined) result['sleep_duration']        = sleep;
        const rawAdc = msg.data['rawAdc'] ?? msg.data[0x0005];
        if (rawAdc !== undefined) result[`raw_adc_sensor_${ep}`] = rawAdc;
        return result;
    },
};

// ─── toZigbee: write calibration / sleep duration ────────────────────────────
// z2m routes the write to the correct endpoint automatically because the
// expose uses .withEndpoint().  For device-level sleep_duration, it goes to ep 1.
const tzCal = {
    key: [
        'sleep_duration',
        'calibration_target',
        'capture_dry',
        'capture_wet',
        ...Array.from({length: NUM_SENSORS}, (_, i) => `cal_dry_sensor_${i + 1}`),
        ...Array.from({length: NUM_SENSORS}, (_, i) => `cal_wet_sensor_${i + 1}`),
    ],
    convertSet: async (entity, key, value, meta) => {
        // ── Calibration target (dropdown) – state only, no device write ───
        if (key === 'calibration_target') {
            return {state: {calibration_target: value}};
        }
        // ── Capture dry / wet calibration point ────────────────────────────
        // Reads the raw ADC from attr 0x0005 of the selected endpoint, then
        // writes it as the dry (0x0001) or wet (0x0002) calibration value.
        // The device must be awake (sleep disabled) for the read to succeed.
        if (key === 'capture_dry' || key === 'capture_wet') {
            if (value !== 'ON' && value !== true) return {state: {[key]: 'OFF'}};
            const target = meta.state.calibration_target || 'sensor_1';
            const epId   = parseInt(target.replace('sensor_', ''), 10);
            if (isNaN(epId) || epId < 1 || epId > NUM_SENSORS)
                throw new Error(`Invalid calibration_target: ${target}`);
            const ep = meta.device.getEndpoint(epId);
            const res    = await ep.read(CAL_CLUSTER_CODE, [0x0005]);
            const rawAdc = res[0x0005] ?? res['rawAdc'];
            if (rawAdc === undefined)
                throw new Error('Could not read rawAdc – ensure device is awake (sleep disabled).');
            const isDry = key === 'capture_dry';
            await ep.write(CAL_CLUSTER_NAME, {[isDry ? 'calDry' : 'calWet']: rawAdc});
            const calKey = `cal_${isDry ? 'dry' : 'wet'}_sensor_${epId}`;
            return {state: {[key]: 'OFF', [calKey]: rawAdc}};
        }
        if (key === 'sleep_duration') {
            const secs = Math.max(60, parseInt(value, 10));   // enforce 60 s minimum
            await entity.write(CAL_CLUSTER_NAME, {sleepDuration: secs});
            return {state: {sleep_duration: secs}};
        }
        const m = key.match(/^cal_(dry|wet)_sensor_(\d+)$/);
        if (m) {
            const attr = m[1] === 'dry' ? 'calDry' : 'calWet';
            await entity.write(CAL_CLUSTER_NAME, {[attr]: parseInt(value, 10)});
            return {state: {[key]: parseInt(value, 10)}};
        }
    },
    convertGet: async (entity, key, meta) => {
        const m    = key.match(/^cal_(dry|wet)_sensor_(\d+)$/);
        const attr = key === 'sleep_duration' ? 'sleepDuration'
                   : m  ? (m[1] === 'dry'    ? 'calDry' : 'calWet')
                   : null;
        if (attr) await entity.read(CAL_CLUSTER_NAME, [attr]);
    },
};

// ─── exposes ──────────────────────────────────────────────────────────────────
const {presets: e, access: ea} = require('zigbee-herdsman-converters/lib/exposes');

function buildExposes() {
    const list = [
        e.battery(),
        e.text('firmware_version', ea.STATE)
            .withDescription('Installed firmware version (e.g. 1.0.0). Updated when device checks for OTA.'),
        e.numeric('sleep_duration', ea.ALL)
            .withUnit('s')
            .withDescription('Deep-sleep interval in seconds (writable). ' +
                             'Minimum 60 s. Takes effect on next wake-up.'),
        // ── Calibration UI ──────────────────────────────────────────────────────
        e.enum('calibration_target', ea.STATE_SET,
               Array.from({length: NUM_SENSORS}, (_, i) => `sensor_${i + 1}`))
            .withDescription('Select which sensor to calibrate, then use the Capture buttons below.'),
        e.binary('capture_dry', ea.SET, 'ON', 'OFF')
            .withDescription('Place the selected sensor in dry air, then press to capture the dry calibration point (0 %). Device must be awake.'),
        e.binary('capture_wet', ea.SET, 'ON', 'OFF')
            .withDescription('Place the selected sensor in water, then press to capture the wet calibration point (100 %). Device must be awake.'),
    ];
    for (let i = 1; i <= NUM_SENSORS; i++) {
        list.push(
            e.numeric(`soil_moisture`, ea.STATE)
                .withUnit('%')
                .withDescription(`Sensor ${i} soil moisture`)
                .withEndpoint(`sensor_${i}`),
            e.numeric('raw_adc', ea.STATE)
                .withDescription(`Sensor ${i} last raw ADC reading (0–4095). Updated each reporting cycle.`)
                .withEndpoint(`sensor_${i}`),
            e.numeric(`cal_dry`, ea.ALL)
                .withDescription(`Sensor ${i}: ADC value in dry air (→ 0 %). ` +
                                 'Place sensor in air and write the raw ADC shown on serial.')
                .withEndpoint(`sensor_${i}`),
            e.numeric(`cal_wet`, ea.ALL)
                .withDescription(`Sensor ${i}: ADC value submerged in water (→ 100 %). ` +
                                 'Place sensor in water and write the raw ADC shown on serial.')
                .withEndpoint(`sensor_${i}`),
        );
    }
    return list;
}

// ─── Endpoint name → ID mapping ───────────────────────────────────────────────
function endpointMap() {
    const map = {};
    for (let i = 1; i <= NUM_SENSORS; i++) map[`sensor_${i}`] = i;
    return map;
}

// ─── Device definition ────────────────────────────────────────────────────────
const definition = {
    zigbeeModel: ['ZB-SoilMeasure'],
    model:       'ZB-SoilMeasure',
    vendor:      'JLT',
    description: 'Zigbee soil moisture sensor – 1 to 9 probes, ADS1115 + onboard ADC, ' +
                 'battery powered, deep-sleep capable.',
    fromZigbee:  [fzMoisture, e.battery ? require('zigbee-herdsman-converters/converters/fromZigbee').battery : null, fzCal, fzFirmwareVersion].filter(Boolean),
    toZigbee:    [tzCal],
    exposes:     buildExposes(),
    ota:         true,
    meta:        {multiEndpoint: true},
    endpoint:    () => endpointMap(),
    // Sleepy end device – it pushes all data on wake-up.
    // An empty configure prevents z2m from attempting cluster binds
    // (which time out because the device is in deep sleep most of the time).
    configure:   async () => {},
};

module.exports = definition;
