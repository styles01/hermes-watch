# Build & flash — Hermes Watch

## Prereqs
- arduino-cli with ESP32 core 3.x
- Libraries: M5Unified (pulls M5GFX) — install via `arduino-cli lib install M5Unified`
- This sketch vendors: KK avatar engine (AGPL, `avatar_engine.*`), a minimal RFC6455
  client (`ws_raw.h`), tiny_websockets (GPL, reference path), M5IOE1/M5PM1 (MIT,
  `firmware/libs/`).
- No other toolchains or package installs are needed.

## Compile
```bash
cd firmware/hermes-watch
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32s3:PartitionScheme=huge_app,PSRAM=opi,FlashSize=16M,USBMode=hwcdc,CDCOnBoot=cdc,DebugLevel=error' \
  --build-path .build \
  --build-property 'build.partitions=partitions_v054.csv' \
  .
```
`partitions_v054.csv` = huge_app + a second 3 MB OTA slot at 0x400000
(NVS/pairing data untouched). The binary must fit 3 MB; current build = 1.23 MB (39%).

## First flash (USB)
```bash
esptool --chip esp32s3 --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0   .build/bootloader/bootloader.bin \
  0x8000 .build/partition_table/partition-table.bin \
  0xe000 .build/bootloader/bootloader_mbedtls_fvt.bin \
  0x10000 .build/hermes-watch.ino.bin
```
(Exact bootloader/mbedtls filenames print at the end of the arduino-cli compile.)
Always verify the app image magic bytes before flashing; take a full backup of the
existing flash contents before your first re-flash of a working device.

## Provisioning (post-flash, via serial 115200)
- WiFi SSID/pass live in NVS namespace `meter` (pair with the companion meter sketch or
  your own tool): keys `ssid`, `pass`, `host`.
- Gateway host: serial command `H <host[:port]>` (persists to NVS `hermes-watch/host`).
- Enrollment + pairing happen automatically on first hub connect; approve the device
  from the gateway side (`docs/GATEWAY.md`).

## Over-the-air (the normal path)
After the first USB flash, ALL updates ship over WiFi: stage + stream + verify + reboot +
probation. See docs/OTA.md. Version tags: `HGVER=` (image) ↔ `Ota::begin(..., version)`.
