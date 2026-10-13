# Building and distributing firmware

How to produce a `.bin` other people can flash to their own ESP32-S3 N16R8, and how they
flash it.

Nothing user-specific is compiled in. Wi-Fi credentials, the Tailscale auth key and the
admin password live in NVS on the device, entered through the setup portal on first boot.
That is what makes the binary shareable.

---

## 1. Prerequisites (you, the builder)

- ESP-IDF **v5.5** at `~/esp/esp-idf` (or set `IDF_PATH`).
- An Xtensa toolchain. Either run `~/esp/esp-idf/install.sh esp32s3`, or rely on the one
  bundled with the Arduino ESP32 core. `build.sh` picks that up automatically from
  `~/Library/Arduino15/packages/esp32/tools/esp-x32/*/bin`. Arduino core 3.3.x ships the
  same `esp-14.2.0_20260121` GCC that IDF 5.5.5 expects.

## 2. Build

```bash
./build.sh
```

Artifacts:

| File | Size | Purpose |
|---|---|---|
| `dist/ts-exit-node-v<ver>-esp32s3-n16r8.bin` | ~1.2 MB | **new board**: bootloader + partition table + otadata + app, pre-combined, flash at `0x0`. **Wipes settings.** |
| `dist/ts-exit-node-v<ver>-esp32s3-n16r8-app.bin` | ~1.0 MB | **upgrade**: the app alone, flash at `0x20000`. Keeps settings and the Tailscale identity. |
| `dist/ts-exit-node-v<ver>.sha256` | | checksums to publish alongside |
| `build/ts-exit-node.bin` | ~1.0 MB | the application alone |
| `build/bootloader/bootloader.bin` | 21 KB | second-stage bootloader |
| `build/partition_table/partition-table.bin` | 3 KB | partition table |
| `build/ota_data_initial.bin` | 8 KB | OTA selector |

The application uses about a third of its 3 MB slot.

The version comes from `PROJECT_VER` in the top-level `CMakeLists.txt`. Bump it there
before publishing.

## 3. Confirm the binary carries no secrets

Do this before publishing. Grep for the **actual values** you used while testing:

```bash
strings dist/ts-exit-node-v1.1.0-esp32s3-n16r8.bin | grep -E "your-ssid|your-wifi-password|tskey-auth-[A-Za-z0-9]"
```

Expect **no output**. (The portal contains the placeholder text `tskey-auth-...`, which the
pattern above does not match.)

Check that `strings` is actually reading the file, so an empty result means "clean". This
should print a non-zero count:

```bash
strings dist/ts-exit-node-v1.1.0-esp32s3-n16r8.bin | grep -c "Tailscale exit node"
```

---

## 4. Instructions for the person flashing

They need [esptool](https://github.com/espressif/esptool) and nothing else:

```bash
pip install esptool
```

### Find the port

| OS | Via a USB-UART bridge | Via the S3's native USB |
|---|---|---|
| macOS | `/dev/cu.usbserial-*`, `/dev/cu.SLAB_USBtoUART` | `/dev/cu.usbmodem*` |
| Linux | `/dev/ttyUSB0` | `/dev/ttyACM0` |
| Windows | `COM3`, `COM4`, … | `COM3`, `COM4`, … |

### Erase, then write

Erasing first avoids inheriting stale settings or an old Tailscale identity:

```bash
esptool --port /dev/cu.usbmodem101 erase-flash
```

One pre-combined file at `0x0`:

```bash
esptool --port /dev/cu.usbmodem101 --baud 460800 write-flash 0x0 ts-exit-node-v1.0.0-esp32s3-n16r8.bin
```

Or the pieces at their offsets. **The app is at `0x20000`, not `0x10000`**, because this
partition table has a 64 KB NVS and two OTA slots:

```bash
esptool --port /dev/cu.usbmodem101 --baud 460800 write-flash 0x0 bootloader.bin 0x8000 partition-table.bin 0x19000 ota_data_initial.bin 0x20000 ts-exit-node.bin
```

`build/flash_args` is the authoritative list if the layout ever changes.

Or both steps from the source tree:

```bash
./build.sh flash /dev/cu.usbmodem101
```

Older esptool (v4 and earlier) uses underscores: `erase_flash`, `write_flash`.

### First boot

See [README.md → First boot](README.md#first-boot). In short: join **`TS-Exit-Setup-XXXX`**,
fill in Wi-Fi + auth key + admin password, then approve the exit node in the Tailscale admin
console.

Watch progress on serial at **115200 baud**:

```bash
idf.py -p /dev/cu.usbmodem101 monitor
```

A failed Wi-Fi join prints every 2.4 GHz network in range and flags whether the configured
SSID is among them. That separates a typo, a 5 GHz-only network and a wrong password.

### GUI alternatives

- [ESP Web Tools](https://esphome.github.io/esp-web-tools/): flashes from Chrome or Edge
  over Web Serial. Point its manifest at the merged `.bin` with offset `0`.
- [Espressif Flash Download Tool](https://www.espressif.com/en/support/download/other-tools):
  Windows GUI. Use the four files and offsets above.

---

## 5. Notes

- **Upgrading: write only the app image at `0x20000`.** That keeps the Wi-Fi config and
  the Tailscale identity:

  ```bash
  esptool --port /dev/cu.usbserial-XXXX --baud 460800 write-flash 0x20000 ts-exit-node-v1.1.0-esp32s3-n16r8-app.bin
  ```

  or `./build.sh upgrade <port>` from the source tree. **Never upgrade with the full
  image.** It starts at `0x0` and pads the gap before `otadata` with `0xFF`, which covers
  NVS at `0x9000`. That wipes the settings and the node identity even without
  `erase-flash`, and the board comes back as a new machine needing a new auth key.
- **To wipe settings and identity without reflashing**, erase just the NVS partition:

  ```bash
  esptool --port /dev/cu.usbmodem101 erase-region 0x9000 0x10000
  ```

- **Coming from WOL-NTFY or another firmware?** Different partition layout. Always
  `erase-flash` first.
