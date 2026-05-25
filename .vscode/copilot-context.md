# Copilot Session Context — JLT ZB-SoilMeasure

Use this file to resume work in a new session.
Last updated: 2026-05-25

---

## Hardware

| Item | Value |
|------|-------|
| MCU | Seeed Studio XIAO ESP32-C6 |
| FQBN | `esp32:esp32:XIAO_ESP32C6:ZigbeeMode=ed,PartitionScheme=zigbee` |
| Board package | esp32 by Espressif 3.3.8 |
| COM port | COM5 |
| Arduino CLI | `C:\Program Files\Arduino CLI\arduino-cli.exe` (v1.5.0) |
| Zigbee role | End Device (ED) |
| Zigbee IEEE addr | `0x58e6c5fffe1a7e18` |
| Battery | 3.7 V 18650 via voltage divider → A0 (GPIO2) |
| User LED | GPIO15 (`LED_BUILTIN`), **active LOW** (LOW = on) |

---

## Firmware versions

| Version | Hex | Status |
|---------|-----|--------|
| v1.0.0 | `0x01000000` | released |
| v1.1.0 | `0x01010000` | **currently on device** (sleep-mode switch, heap-exhaustion fix, stack overflow fix) |
| v1.2.0 | `0x01020000` | **OTA pending** — adds User LED endpoint 4 (ZigbeeLight, GPIO15 active-low) |

### Build & upload commands
```powershell
# Compile
& "C:\Program Files\Arduino CLI\arduino-cli.exe" compile `
  --fqbn "esp32:esp32:XIAO_ESP32C6:ZigbeeMode=ed,PartitionScheme=zigbee" `
  "C:\Users\lars.skogs\Source\JLT-ZB-SoilMeasure\XIAO-ESP32-C6-SoilMonitor"

# Upload
& "C:\Program Files\Arduino CLI\arduino-cli.exe" upload `
  --fqbn "esp32:esp32:XIAO_ESP32C6:ZigbeeMode=ed,PartitionScheme=zigbee" `
  --port COM5 `
  "C:\Users\lars.skogs\Source\JLT-ZB-SoilMeasure\XIAO-ESP32-C6-SoilMonitor"

# Serial monitor
& "C:\Program Files\Arduino CLI\arduino-cli.exe" monitor --port COM5 --config baudrate=115200
```

### ELF / disassembly tools (for crash analysis)
```
C:\Users\lars.skogs\AppData\Local\Arduino15\packages\esp32\tools\esp-rv32\2601\bin\riscv32-esp-elf-{addr2line,objdump,nm}.exe
ELF: XIAO-ESP32-C6-SoilMonitor\build\XIAO-ESP32-C6-SoilMonitor.ino.elf
```

---

## Zigbee endpoint map

| EP | Class | Purpose |
|----|-------|---------|
| 1 | ZigbeeSoilSensor | Sensor 1 — humidity + calibration cluster 0xFC11 + OTA client |
| 2 | ZigbeeSoilSensor | Sensor 2 |
| 3 | ZigbeeSoilSensor | Sensor 3 |
| 4 | ZigbeeLight | User LED (GPIO15, active low) — **added in v1.2.0** |

### Custom calibration cluster 0xFC11 ("jltSoilCal")
| Attr | Name | Type | Notes |
|------|------|------|-------|
| 0x0001 | calDry | U16 | ADC in dry air → 0% |
| 0x0002 | calWet | U16 | ADC in water → 100% |
| 0x0003 | sleepDuration | U32 | Deep-sleep interval (seconds) |
| 0x0004 | sleepEnabled | U8 | 0=awake loop, 1=deep sleep |

NVS namespace `soil_cal`: keys `sleep_en` (bool), `sleep_sec` (u32)

---

## OTA release process

CI workflow (`.github/workflows/build.yml`) triggers on tag `v*`:
1. Parses `v1.2.0` → patches `OTA_RUNNING_VERSION` to `0x01020000u` in `config.h`
2. Compiles firmware
3. Runs `tools/create_ota_image.py` → `build/JLT-ZB-SoilMeasure.zigbee`
4. Creates GitHub Release with stable URL: `/releases/latest/download/JLT-ZB-SoilMeasure.zigbee`

**IMPORTANT**: When pushing multiple tags at once, the CI jobs race and the wrong
release may become "Latest". Fix with:
```powershell
gh release edit v1.2.0 --repo JLTSkogshus/JLT-ZB-SoilMeasure --latest
```

OTA index: `ota/index.json` — points to `/releases/latest/download/`.
Do NOT manually change `OTA_RUNNING_VERSION` in `config.h` before tagging — CI does it.

---

## z2m converter

File: `zigbee2mqtt/JLT-ZB-SoilMeasure.mjs`

Deploy via z2m frontend: **Settings → Dev console → External converters → paste content**.

Key design decisions:
- `sleep_duration` / `sleep_enabled` always write to **endpoint 1** via numeric IDs (0xFC11) to avoid UNSUPPORTED_CLUSTER when entity is not an endpoint
- `fzFirmwareVersion` catches `commandQueryNextImageRequest` (not just attributeReport) because the device sends its version inside the OTA Query Next Image Request ZCL command, not as an attribute report
- `fzLed` filters to `msg.endpoint.ID === 4` to avoid false positives from other genOnOff traffic
- `tzLed.convertSet` routes directly to `meta.device.getEndpoint(4)` for same reason

---

## Known issues / next steps

- [ ] **Re-interview device after v1.2.0 OTA lands** — z2m needs to rediscover endpoint 4
- [ ] **Verify User LED toggle** works after re-interview (check LED polarity — if inverted, swap LOW/HIGH in `onLedChange()` in `XIAO-ESP32-C6-SoilMonitor.ino`)
- [ ] **Verify firmware_version expose** populates to "1.2.0" after next OTA check
- [ ] **Sleep mode end-to-end test** — toggle `sleep_enabled` ON, verify device sleeps and wakes on schedule
- [ ] Sensor calibration values are factory defaults (dry=3200, wet=1400) — calibrate with real soil if needed

---

## Key fixes made (this session series)

| Fix | File(s) |
|-----|---------|
| Stack overflow → 16 KB loop task | `XIAO-ESP32-C6-SoilMonitor.ino` |
| Heap exhaustion crash (9→3 global sensor objects) | `XIAO-ESP32-C6-SoilMonitor.ino` |
| `setPowerSource` at runtime → `setBatteryPercentage` | `XIAO-ESP32-C6-SoilMonitor.ino` |
| Sleep-mode NVS switch (cluster 0xFC11 attr 0x0004) | `zigbee_soil_sensor.*`, `calibration.*` |
| z2m UNSUPPORTED_CLUSTER → numeric cluster/attr IDs | `JLT-ZB-SoilMeasure.mjs` |
| User LED ZigbeeLight endpoint 4 | `XIAO-ESP32-C6-SoilMonitor.ino`, `JLT-ZB-SoilMeasure.mjs` |
| firmware_version expose catches OTA query command | `JLT-ZB-SoilMeasure.mjs` |

---

## GitHub
Remote: `https://github.com/JLTSkogshus/JLT-ZB-SoilMeasure.git`
Branch: `master`
