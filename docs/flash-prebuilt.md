# Flash precompiled firmware without ESP-IDF

[English](flash-prebuilt.md) · [简体中文](flash-prebuilt.zh-CN.md) · [日本語](flash-prebuilt.ja.md)

Flash **ESP32-S3 N16R8** on Windows 11 or Linux with Espressif's official tools. You do not need Git, CMake, Ninja, ESP-IDF, its compiler, or a project-specific flashing script. Windows also has an official graphical tool that does not require Python. To change compiled defaults, use [build and flash from source](build-and-flash.md).

## 1. Choose the right image

Obtain files from a trusted [project release](https://github.com/noaic/esp32-socks5-gateway/releases), or the firmware artifact of a successful **Verify and package firmware** Actions run. Access to the repository is required. If none is available, build from source. The workflow uploads artifacts; it does not automatically publish a Release.

| File | Use |
| --- | --- |
| `factory.bin` | Complete initial-install/reset image: bootloader + partition table + application. Write at **`0x0`**. **Resets saved network and management settings** |
| `app.bin` | Application-only update. Write at **`0x10000`**, only with a compatible installed bootloader, partition layout and settings version |
| `firmware.zip` | Convenient package of both images, original split images, metadata, instructions and licenses; **no host scripts** |
| `manifest.json`, `SHA256SUMS` | Hardware/build provenance and SHA-256 checksums |
| `firmware.zip.sha256` | Checksum for the ZIP itself; distributed alongside the archive |
| `FLASHING.md`, `LICENSE`, `THIRD_PARTY_NOTICES.md`, `LICENSES/` | Instructions and applicable licenses; keep these when redistributing firmware |

The complete image is merged from build outputs, **not a 16 MB dump of a configured device**. It ends after the application instead of padding the entire Flash. Gaps between images contain `0xFF`, including NVS at `0x9000–0xefff`: writing `factory.bin` overwrites those saved settings even without `erase-flash`. Choose `app.bin` for a compatible update that should retain configuration. An app image alone cannot prepare a blank board.

**Hardware:** ESP32-S3 with exactly 16 MB Flash and 8 MB Octal PSRAM, a data USB cable and stable power. Images use DIO / 80 MHz Flash. The firmware checks PSRAM at startup. Default LED is WS2812 on GPIO48; disable it in management if unsuitable. Neither a filename nor chip detection verifies every board wiring difference.

Download `factory.bin` or `app.bin` with `SHA256SUMS` and the notices, or extract `firmware.zip` into a new empty folder. If an Actions download wraps the firmware ZIP, extract both layers. The examples below assume the extracted files are in `gateway-firmware`. Checksum comparison detects corruption, not publisher identity; use a trusted download source.

## 2. Windows 11: PowerShell

### Install official esptool and verify the image

Install **Python ≥ 3.10** from [python.org](https://www.python.org/downloads/windows/) if needed. Use ordinary PowerShell, without an ESP-IDF environment. If your installation provides `py`, replace initial `python` commands with `py -3`.

For example, extract the downloaded `firmware.zip` and create an isolated tool environment:

```powershell
New-Item -ItemType Directory -Force C:\esp | Out-Null
Expand-Archive -LiteralPath "$env:USERPROFILE\Downloads\firmware.zip" -DestinationPath C:\esp\gateway-firmware
Set-Location C:\esp\gateway-firmware
Get-FileHash -Algorithm SHA256 .\factory.bin
Get-Content .\SHA256SUMS | Select-String '  factory\.bin$'
python --version
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install esptool==5.5.0
```

Compare the two SHA-256 values; letter case is irrelevant. For an update, check `app.bin` instead. To check the ZIP before extraction, use `Get-FileHash` on that ZIP and compare with `firmware.zip.sha256`. Calling the virtual environment's Python directly avoids activation-policy changes. These commands use the official installed esptool module, not a script from the firmware package.

### Select the serial port and flash

Connect the board and compare **Device Manager → Ports (COM & LPT)** before/after connection. Install the USB bridge manufacturer's driver if needed. Replace `COM5` below, and close other programs using the port. Check that Flash detection reports **16 MB**:

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 flash-id
```

For **first installation or an intentional settings reset**:

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 factory.bin
```

For an **application update with compatible existing bootloader/partitions/settings**, use this command instead:

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x10000 app.bin
```

No separate full-Flash erase is needed. After flashing, optionally open the UART0 console:

```powershell
.\.venv\Scripts\python.exe -m serial.tools.miniterm COM5 115200 --raw
```

Exit with `Ctrl+]`. A native USB flashing port may differ from the UART / USB-UART console port.

### Alternative: official graphical tool, without Python

Download Espressif's [Flash Download Tool](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32s3/production_stage/tools/flash_download_tool.html). Select **ESP32-S3**, the board's download connection and COM port. Add `factory.bin` at **`0x0`**, select its checkbox, keep **DoNotChgBin** enabled to preserve the image's DIO / 80 MHz / 16 MB parameters, and start at 115200 baud. This initial-install image resets configuration. A compatible application update uses only `app.bin` at `0x10000`. Do not use the erase function for an ordinary update.

## 3. Linux: Bash

Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install python3 python3-venv unzip
```

Fedora (`venv` is included with Python):

```bash
sudo dnf install python3 python3-pip unzip
```

Use Python ≥ 3.10. Extract and verify the package, then install the official tool:

```bash
mkdir -p ~/esp/gateway-firmware
unzip ~/Downloads/firmware.zip -d ~/esp/gateway-firmware
cd ~/esp/gateway-firmware
sha256sum -c SHA256SUMS
python3 --version
python3 -m venv .venv
.venv/bin/python -m pip install esptool==5.5.0
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

`sha256sum -c` checks every listed payload file; all must report `OK`. If downloading a single BIN instead, compare `sha256sum factory.bin` (or `app.bin`) with its line in `SHA256SUMS`. For the archive, run `sha256sum -c firmware.zip.sha256` in the folder containing that ZIP.

Replace `/dev/ttyACM0` with your port. Check `ls -l /dev/ttyACM0`; for a `dialout`-owned device, use `sudo usermod -aG dialout "$USER"` and log out/in. Follow your distribution's rules for other groups; avoid routinely flashing as root. Confirm **16 MB** before writing:

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 flash-id
```

For **first installation/settings reset**:

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x0 factory.bin
```

For a **compatible app-only update**, use this instead:

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x10000 app.bin
```

Optional UART0 console:

```bash
.venv/bin/python -m serial.tools.miniterm /dev/ttyACM0 115200 --raw
```

Exit with `Ctrl+]`. **WSL2:** a Windows COM port is not automatically a Linux serial device. Follow the [WSL2 USB attachment steps](build-and-flash.md#4-flash-the-source-built-firmware), then use its `/dev/tty*` path. Native Windows flashing avoids this extra step.

## 4. First connection, updates and troubleshooting

After an initial installation, the default AP is `ESP32-Transparent-Proxy`, its password is `esp32proxy`, and management is at **http://192.168.4.1/**. No upstream Wi-Fi is preconfigured. Set a management password and your Wi-Fi/SOCKS5 settings, and change the shared AP password before normal use. Follow [first-time setup](../README.md#flash-and-connect-for-the-first-time), including **confirmation within 180 seconds** after saving network settings.

App-only updates normally retain NVS and its saved values take precedence over compiled defaults. Check the release's compatibility notes first: changed partitions, incompatible settings or firmware NVS recovery can still require/reset configuration. `factory.bin` always replaces the NVS range in this layout. Save your settings before choosing a reset; the firmware ZIP is not a configuration backup. See [partitions and reflashing](build-and-flash.md#partitions-images-and-reflashing).

Start at 115200 baud; `--baud 460800` is optional after a reliable first flash. If connection fails, check the data cable, driver, port, USB connector, permissions and other serial programs. Follow the board's BOOT/EN instructions if automatic download-mode entry fails. Do not force an image past a chip/Flash/layout mismatch.

### Advanced: separate images and optional helper

The ZIP also includes the original three images. When the partition layout is compatible, official esptool can update all three without writing the NVS gap. With esptool installed, run the following from the extracted directory, replacing `COM5` with the actual Windows or Linux port:

```text
esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 images/bootloader.bin 0x8000 images/partition-table.bin 0x10000 images/esp32-socks5-gateway.bin
```

If the `esptool` command is not on your PATH, use the virtual environment's Python followed by `-m esptool`, as above. The optional project helper remains in the source checkout at `tools/flash_firmware.py`; it validates the package and writes these split images. It is **not included in the download and is not required**. See [tool usage](../tools/README.md).

## Build a package as a maintainer

From an activated ESP-IDF terminal in the project root, after the [source-build steps](build-and-flash.md):

```text
python tools/package_firmware.py --build-dir build --output-dir dist/esp32-socks5-gateway-n16r8-local
```

This creates a new output directory with the images, checksums, notices and script-free `firmware.zip`. Use a clean public-default configuration; never distribute private compiled credentials, device NVS dumps or personal logs. Review the manifest and licenses before sharing. Packaging and artifact generation do not upload a Release. See [tool usage](../tools/README.md).

## Verification scope

Package generation, integrity/failure handling and command construction are covered by host tests and Linux build validation. Official esptool can inspect the generated images without hardware. Windows/WSL2 and graphical-tool instructions follow official interfaces; this cloud environment has no board and does not verify real Windows flashing or ESP32-S3 boot. Historical radio measurements remain in [performance and validation](performance.md).
