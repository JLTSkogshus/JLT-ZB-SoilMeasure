# XIAO ESP32-C6 Soil Moisture Monitor

Battery-powered Zigbee soil moisture sensor for Home Assistant via zigbee2mqtt.

## Hardware

| Component | Details |
|---|---|
| MCU | Seeed Studio XIAO ESP32-C6 |
| Sensors | 1–9 × Capacitive Soil Moisture Sensor V1.2 |
| Battery | 3.7 V 18650 Li-Ion 2000 mAh |

### Wiring

#### ADS1115 over I2C (primary ADC – up to 16 channels across 4 chips)

| XIAO ESP32-C6 | ADS1115 |
|---|---|
| D4 / A4 / GPIO6 (SDA) | SDA |
| D5 / A5 / GPIO7 (SCL) | SCL |
| 3.3 V | VDD |
| GND | GND |

> **A4 and A5 are reserved for I2C — they cannot be used as analog inputs.**

Set the ADDR pin on each ADS1115 chip to select a unique I2C address:

| ADDR pin | I2C address | Used for |
|---|---|---|
| GND | 0x48 | Sensors 1–4 (ch0–3) |
| VDD | 0x49 | Sensors 5–8 (ch0–3) |
| SDA | 0x4A | optional 3rd chip |
| SCL | 0x4B | optional 4th chip |

#### Onboard ADC pins (1–2 additional sensors)

| Sensor # | XIAO Pin | GPIO | Note |
|---|---|---|---|
| Optional 1 | A1 | GPIO3 | configured as S3 by default |
| Optional 2 | A2 | GPIO4 | configured as S9 by default |

Edit `SENSOR_ADC_CONFIG[]` in `config.h` to remap any slot to any source.

For 7–9 sensors, add a second ADS1115 (addr 0x49) — no extra wiring needed beyond I2C.

#### Battery ADC (voltage divider — required)
```
Vbat (4.2 V) ──[100 kΩ]──┬──[100 kΩ]── GND
                           └── A0 (GPIO2)
```
This scales 4.2 V → 2.1 V, safely within the ESP32-C6's 3.3 V ADC range.
A1–A3 may be used instead; update `BATTERY_ADC_PIN` in `config.h`.

---

## Software Setup

### 1. Install the ESP32 board package
In Arduino IDE: **File → Preferences → Additional board manager URLs**, add:
```
https://espressif.github.io/arduino-esp32/package_esp32_index.json
```
Then **Tools → Board → Boards Manager**, search `esp32`, install **≥ 3.0.0**.

### 2. Board & Zigbee settings
| Setting | Value |
|---|---|
| Board | Seeed Studio XIAO ESP32C6 |
| Partition Scheme | Zigbee |
| Zigbee Mode | **Zigbee ED (end device)** ← critical |

### 3. Configure `config.h`
- Set `NUM_SENSORS` to the number of physically connected sensors.
- Edit `SENSOR_ADC_CONFIG[]` to assign each sensor slot to an ADS1115 channel or onboard pin.
- Calibration is done **over Zigbee** (no reflash needed) — see calibration section below.
- Adjust `SLEEP_DURATION_SEC` (default 900 s = 15 minutes).

### 4. Install Adafruit ADS1X15 library
In Arduino IDE: **Sketch → Include Library → Manage Libraries**, search `Adafruit ADS1X15`, install.

### 4. Upload
Select the correct COM port and click **Upload**.

---

## Home Assistant / zigbee2mqtt

After pairing, each sensor endpoint appears as a **humidity** entity.
Rename them to "Soil Moisture – Pot 1", "Soil Moisture – Pot 2", etc.
Battery % is reported on the first endpoint and appears automatically.

### Entities per device
| Entity | Source |
|---|---|
| `sensor.soil_moisture_1` … `_N` | Relative Humidity cluster (0x0405) |
| `sensor.battery` | Power Configuration cluster (0x0001) |

### Timing info (Serial log)
| Info | Description |
|---|---|
| Boot count | Total wake-ups since power-on |
| Last connection | Elapsed seconds at last Zigbee TX |
| Next wakeup | Elapsed seconds at scheduled next wake |
| Sleep duration | Configured via `SLEEP_DURATION_SEC` |

---

## Project Structure
```
XIAO-ESP32-C6-SoilMonitor/
├── XIAO-ESP32-C6-SoilMonitor.ino   # Main sketch
├── config.h                         # All user-adjustable settings
├── adc_reader.h / adc_reader.cpp    # ADC abstraction (onboard + ADS1115)
├── calibration.h / calibration.cpp  # Per-sensor NVS calibration
├── README.md                        # This file
└── .vscode/
    └── arduino.json                 # VS Code Arduino extension settings
```

---

## License
MIT