#!/usr/bin/env bash
# Builds the ONE generic firmware used for every unit (no per-device secrets inside) with arduino-cli.
# Output: firmware/soundbox_consumer.merged.bin  (bootloader + partitions + app, flash at 0x0)
# scripts/onboard_device.py then patches each unit's provisioning image into a copy of it.
#
# Override the board options if yours differs, e.g.:
#   FQBN="esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=default_16MB,CDCOnBoot=cdc" scripts/build-firmware.sh
# The NVS partition of the chosen partition scheme must match onboard_device.py --nvs-offset/--nvs-size
# (0x9000 / 0x5000 for the stock "default" schemes).
set -euo pipefail
cd "$(dirname "$0")/.."
command -v arduino-cli >/dev/null || { echo "arduino-cli not found: https://arduino.github.io/arduino-cli/"; exit 1; }

FQBN="${FQBN:-esp32:esp32:esp32s3:PartitionScheme=default}"
arduino-cli core install esp32:esp32
arduino-cli lib install "U8g2" "ArduinoJson@^7"

out=firmware/build
rm -rf "$out"; mkdir -p "$out"
arduino-cli compile --fqbn "$FQBN" --output-dir "$out" sketch/soundbox_consumer

merged=$(ls "$out"/*.merged.bin 2>/dev/null | head -1)
[[ -n "$merged" ]] || { echo "no merged.bin produced; check the build output"; exit 1; }
cp "$merged" firmware/soundbox_consumer.merged.bin
shasum -a 256 firmware/soundbox_consumer.merged.bin | tee firmware/soundbox_consumer.merged.bin.sha256
echo "Built firmware/soundbox_consumer.merged.bin"
