#!/usr/bin/env python3
"""
create_ota_image.py  –  Wrap an ESP32 Arduino .bin into a Zigbee OTA image.

Usage
-----
    python tools/create_ota_image.py  <input.bin>  <new_version>  [output.zigbee]

Arguments
---------
    input.bin     Compiled Arduino binary (produced by arduino-cli compile
                  --output-dir <dir> …).  Typically named
                  XIAO-ESP32-C6-SoilMonitor.ino.bin
    new_version   Dotted-decimal version string, e.g. 1.1.0.0
                  Must be HIGHER than OTA_RUNNING_VERSION in config.h so that
                  the device accepts the update.
    output.zigbee (optional) Output file path.
                  Defaults to <input base>_v<version>.zigbee

Zigbee OTA file format (ZCL spec §11.4)
----------------------------------------
  Offset  Size   Field
  ------  ----   -----
   0       4     Tag identifier    = 0x0BEEF11E
   4       2     Header version    = 0x0100
   6       2     Header length     = 56 (no optional fields)
   8       2     Field control     = 0x0000
  10       2     Manufacturer code = 0x1001   ← must match addOTAClient()
  12       2     Image type        = 0x1011   ← must match addOTAClient()
  14       4     File version      (encoded from <new_version>)
  18       2     Zigbee stack ver  = 0x0002
  20      32     Header string     "JLT ZB-SoilMeasure" padded to 32 bytes
  52       4     Total image size  = 56 + len(binary)
  56       …     Binary payload    (the .bin file content)

Place the resulting .zigbee file in your zigbee2mqtt data directory and use
z2m's OTA management interface to push it to the device.

zigbee2mqtt OTA setup (configuration.yaml)
-------------------------------------------
    ota:
      update_check_interval: 1440    # minutes between auto-check
      disable_automatic_update_check: false

Then in the z2m web UI:  Devices → JLT ZB-SoilMeasure → OTA → Perform update.
"""

import struct
import sys
import os
import re

# ── Constants matching addOTAClient() defaults in ZigbeeEP.h ─────────────────
MANUFACTURER_CODE = 0x1001
IMAGE_TYPE        = 0x1011
TAG_IDENTIFIER    = 0x0BEEF11E
HEADER_VERSION    = 0x0100
ZIGBEE_STACK_VER  = 0x0002
HEADER_LENGTH     = 56
HEADER_STRING     = b"JLT ZB-SoilMeasure"   # max 32 bytes


def parse_version(ver_str: str) -> int:
    """Convert 'major.minor.patch.build' → uint32 0xMMNNPPBB."""
    parts = [int(x) for x in re.split(r'[.\-]', ver_str)]
    while len(parts) < 4:
        parts.append(0)
    major, minor, patch, build = parts[:4]
    for name, val in [("major", major), ("minor", minor),
                      ("patch", patch), ("build", build)]:
        if not 0 <= val <= 255:
            raise ValueError(f"{name} component {val} out of 0–255 range")
    return (major << 24) | (minor << 16) | (patch << 8) | build


def build_ota_header(file_version: int, total_size: int) -> bytes:
    header_str = HEADER_STRING[:32].ljust(32, b'\x00')
    return struct.pack(
        "<IHHHHHIH32sI",
        TAG_IDENTIFIER,
        HEADER_VERSION,
        HEADER_LENGTH,
        0x0000,           # field control
        MANUFACTURER_CODE,
        IMAGE_TYPE,
        file_version,
        ZIGBEE_STACK_VER,
        header_str,
        total_size,
    )


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    bin_path = sys.argv[1]
    ver_str  = sys.argv[2]
    out_path = sys.argv[3] if len(sys.argv) > 3 else None

    # Validate input
    if not os.path.isfile(bin_path):
        print(f"ERROR: '{bin_path}' not found.", file=sys.stderr)
        sys.exit(1)

    try:
        file_version = parse_version(ver_str)
    except ValueError as e:
        print(f"ERROR: bad version '{ver_str}': {e}", file=sys.stderr)
        sys.exit(1)

    with open(bin_path, "rb") as f:
        binary = f.read()

    total_size = HEADER_LENGTH + len(binary)
    header     = build_ota_header(file_version, total_size)
    assert len(header) == HEADER_LENGTH, f"Header is {len(header)} bytes, expected {HEADER_LENGTH}"

    if out_path is None:
        base     = os.path.splitext(bin_path)[0]
        out_path = f"{base}_v{ver_str.replace('.', '_')}.zigbee"

    with open(out_path, "wb") as f:
        f.write(header)
        f.write(binary)

    print(f"OTA image created:")
    print(f"  Input  : {bin_path}  ({len(binary):,} bytes)")
    print(f"  Output : {out_path}  ({total_size:,} bytes)")
    print(f"  Version: {ver_str}  (0x{file_version:08X})")
    print(f"  MFR    : 0x{MANUFACTURER_CODE:04X}")
    print(f"  Type   : 0x{IMAGE_TYPE:04X}")
    print()
    print("Next steps:")
    print("  1. Copy the .zigbee file to your zigbee2mqtt data directory.")
    print("  2. In z2m web UI: Devices → JLT ZB-SoilMeasure → OTA → Perform update.")
    print("  3. Wait for the device to wake up – it checks for OTA on every connect.")


if __name__ == "__main__":
    main()
