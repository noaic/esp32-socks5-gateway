# Build and flash from source

[English](build-and-flash.md) · [简体中文](build-and-flash.zh-CN.md) · [日本語](build-and-flash.ja.md)

This guide keeps the exact SDK, lwIP preparation, configuration, and flashing steps together. For ordinary use without ESP-IDF, choose the [prebuilt firmware guide](flash-prebuilt.md). After flashing, return to [first-time setup](../README.md#flash-and-connect-for-the-first-time).

## Development dependencies

### Software

| Category | Dependency and purpose | Preparation |
| --- | --- | --- |
| Required: development tools | Git, Python ≥ 3.10 (3.12 recommended), CMake ≥ 3.22, Ninja | Official EIM checks/installs prerequisites on Windows; Linux packages are listed below. This IDF tool manifest recommends CMake 4.0.3 and Ninja 1.12.1 |
| Required: SDK | Exact ESP-IDF v6.1 and esp-lwIP commits | See the commit table below; FreeRTOS and lwIP are included in ESP-IDF, not separate installations |
| Automatically managed: toolchain | Xtensa GCC 15.2.0 for ESP32-S3 firmware; esptool, Python build tools, Component Manager | EIM or IDF scripts use this commit's tool/requirement manifests |
| Automatically managed: components | cJSON 1.7.19, led_strip 3.0.3 | Declared in `main/idf_component.yml`, version/hash-pinned in `dependencies.lock`, downloaded into `managed_components/` at first build |
| Required: use of the gateway | Browser, existing 2.4 GHz Wi-Fi, IPv4 / NO AUTH / UDP ASSOCIATE SOCKS5 service | User-provided; building and host tests do not require a live proxy |
| Optional: development/testing | Linux system GCC/binutils, Node.js; ccache speeds repeated builds | GCC runs host C tests, Node.js runs frontend tests; neither is a firmware runtime dependency |

The management and first-use pages default to English. A Chinese first browser language (`zh` or `zh-*`) selects Simplified Chinese; other or unavailable preferences use English. A secondary Chinese preference does not override an English first preference. Serial commands are in English. System GCC and the ESP32 cross-compiler serve different purposes. Do not substitute the former for the firmware toolchain or update the dependency lock merely to make a build pass.

Use **PowerShell on Windows**, and **Bash on Linux/WSL2**; run only the commands for your platform. Avoid spaces and non-ASCII characters in SDK/project paths, for example by using `C:\esp` on Windows. These steps prepare a separate SDK directory without overwriting an installation used by other projects.

## 1. Install the matching ESP-IDF revision

Transparent forwarding uses a small lwIP patch. lwIP is the TCP/IP networking library inside ESP-IDF. The patch depends on its source code, so a version string of “v6.1” alone is insufficient; these commits must match:

| Dependency | Exact commit |
| --- | --- |
| ESP-IDF v6.1 | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| esp-lwIP | `c6f2f878e7b0f86033214b85547d579be43351e3` |

### Windows 11: official EIM and PowerShell

1. Download the EIM GUI installer from [Espressif](https://dl.espressif.com/dl/eim/). Follow the prerequisite checks in the [official Windows guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/windows-setup.html). If Git is unavailable, install [Git for Windows](https://git-scm.com/install/windows) and open a new PowerShell. `winget` is not required.
2. In ordinary PowerShell, put the exact commit in a **new** `C:\esp\esp-idf-gateway` directory. The preparation script accepts both LF and CRLF checkouts; no Git line-ending settings need to be changed:

```powershell
git --version
New-Item -ItemType Directory -Force C:\esp | Out-Null
git clone --no-checkout --depth 1 https://github.com/espressif/esp-idf.git C:\esp\esp-idf-gateway
Set-Location C:\esp\esp-idf-gateway
git fetch --depth 1 origin fff9895c82d744c7237be8847347bdd1b07c6643
git checkout --detach fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive --depth 1
```

3. In EIM, choose **Custom Installation** and point its path to this existing Git directory. Official [Expert Installation](https://docs.espressif.com/projects/idf-im-ui/en/latest/expert_installation.html) supports using an existing repository without rewriting its contents. Install the tools for that checkout, including the compiler, CMake, Ninja, and Python environment. Use this directory rather than installing an arbitrary latest SDK for the build.
4. Open the installation's PowerShell through EIM's **Manage Installations → Open IDF Terminal**, or its generated PowerShell shortcut. **Run subsequent Windows commands in that activated terminal.** A new ordinary PowerShell does not inherit the environment:

```powershell
python --version
idf.py --version
git -C "$env:IDF_PATH" rev-parse HEAD
git -C "$env:IDF_PATH\components\lwip\lwip" rev-parse HEAD
git -C "$env:IDF_PATH\components\lwip\lwip" ls-files --eol src/core/ipv4/ip4.c
cmake --version
ninja --version
```

Both commits must match the table. The EOL command is diagnostic: `i/lf w/lf` and `i/lf w/crlf` are both supported; the latter means Git checked out Windows line endings, not that the source was edited. An existing SDK need not be reinstalled for this difference. Use EIM's generated activation entry point. For legacy script installs, PowerShell runs `.\install.ps1 esp32s3` and `.\export.ps1` from the SDK directory; CMD uses `install.bat esp32s3` / `export.bat`. Windows does not use Bash's `source`.

### Linux: Ubuntu/Debian or Fedora, Bash

For Ubuntu/Debian, use the [official prerequisite list](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/linux-macos-setup-legacy.html). Choose a release providing Python ≥ 3.10, such as Ubuntu 22.04 or newer:

```bash
sudo apt-get update
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

Fedora uses different package names and `dnf`; its Python package provides `venv`:

```bash
sudo dnf install git wget flex bison gperf python3 python3-pip cmake ninja-build ccache libffi-devel openssl-devel dfu-util libusb1
```

Prepare a separate SDK directory; `esp-idf-gateway` must not already exist:

```bash
mkdir -p ~/esp
cd ~/esp

git clone --no-checkout --depth 1 https://github.com/espressif/esp-idf.git esp-idf-gateway
cd esp-idf-gateway
git fetch --depth 1 origin fff9895c82d744c7237be8847347bdd1b07c6643
git checkout --detach fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive --depth 1

./install.sh esp32s3
python3 tools/idf_tools.py install cmake ninja
source ./export.sh
```

Installation downloads the matching compiler and Python dependencies. Explicitly requesting CMake/Ninja avoids outdated system versions. **Activate again in every new Bash**, then inspect the actual versions:

```bash
source ~/esp/esp-idf-gateway/export.sh
idf.py --version
git -C "$IDF_PATH" rev-parse HEAD
git -C "$IDF_PATH/components/lwip/lwip" rev-parse HEAD
cmake --version
ninja --version
```

### Can an existing installation be reused?

Reuse an SDK only when both commits match, submodules are complete, and global lwIP source is unchanged. Activate it through its own entry point and run the checks above; “v6.1” alone is insufficient. Otherwise prepare a separate SDK, rather than running `git pull` or changing versions in a shared installation. Use EIM terminals for EIM installations and the matching `export` script for script installations.

A shallow clone may make `idf.py --version` report `Git version unavailable` before reading the source version. Validate both exact commits and the patch checks rather than relying on a tag description.

For WSL2, install inside the Linux distribution using the Linux steps; Windows compiler binaries cannot be reused directly. USB attachment is covered below. macOS users can consult the official guide; Bash project commands are the same.

## 2. Get the project and prepare lwIP

Windows, in activated PowerShell:

```powershell
git clone https://github.com/noaic/esp32-socks5-gateway.git C:\esp\esp32-socks5-gateway
Set-Location C:\esp\esp32-socks5-gateway
python tools/prepare_lwip.py apply
python tools/prepare_lwip.py check
```

Linux/WSL2, in activated Bash:

```bash
git clone https://github.com/noaic/esp32-socks5-gateway.git ~/esp32-socks5-gateway
cd ~/esp32-socks5-gateway
python3 tools/prepare_lwip.py apply
python3 tools/prepare_lwip.py check
```

If you already have the project, enter that checkout rather than cloning again. All subsequent project commands run from the **project root, containing `CMakeLists.txt`**.

`patches/lwip-v6.1-transparent-tcp.patch` contains the `ip4.c` changes; `patches/lwip-v6.1.json` records the commits and original/patched SHA-256 hashes. The script copies the entire `$IDF_PATH/components/lwip` component, including its lwIP submodule, into project `components/lwip` and modifies only that copy. ESP-IDF's [same-name component precedence](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/build-system.html#multiple-components-with-the-same-name) selects the project override, leaving the global SDK and other projects unaffected. The generated directory is Git-ignored.

For `ip4.c` hash checks, CRLF is treated as LF in memory; the pinned hashes are unchanged. `apply` and `restore` write LF only to the verified project copy, and CRLF patch input is also supported without rewriting the patch file. `check` is read-only. Before copying or checking, `apply`/`check` also compare the **whole global lwIP component and submodule against their committed Git contents**, rejecting edited, missing, or extra source files even when HEAD matches. Text files allow LF/CRLF checkout differences; binary files must match byte for byte. Code/whitespace edits, a BOM, or lone CR differences are rejected. The local override must then match that verified SDK copy apart from patched `ip4.c`.

These commands work in either activated terminal (Linux can also use `python3`):

| Command | Purpose |
| --- | --- |
| `python tools/prepare_lwip.py apply` | Check revisions/content, create or reuse the local copy, normalize its `ip4.c` to LF, and patch it; safe to repeat |
| `python tools/prepare_lwip.py check` | Read-only verification of revisions, LF-normalized original/patched hashes, and the complete component file list/content |
| `python tools/prepare_lwip.py restore` | Reverse the verified local `ip4.c` patch and leave it as LF; no activated SDK needed. Keep the local component and do not disable transparent mode |

`restore` verifies only the local `ip4.c` patch state; it is an offline reversal, not a full SDK/component integrity check. Run `apply` and `check` for the full verification.

Default transparent mode requires another `apply` and `check` after `restore`; `check` rejects an unpatched copy. If an existing build used global lwIP, run `idf.py reconfigure` after preparing the override, then build. Revision errors identify the expected and actual commits; a hash error means changes beyond LF/CRLF. Back up and inspect unexpected SDK/local edits, or prepare a separate clean SDK; do not force overwrites. `apply`/`check` also reject other component changes, and writes through local symlinks are refused. An ESP-IDF/esp-lwIP upgrade may change the `ip4.c` path, hook placement, or interfaces, requiring a separate patch review/adaptation. Do not just change commits/hashes to bypass validation.

## 3. Configure and build

These configuration/build commands are the same in either activated terminal:

```text
idf.py menuconfig
```

Open `Transparent gateway Wi-Fi`:

- For a first setup, you can leave the upstream `SSID` empty and configure it later on the management page. The AP starts, but the ESP32 does not attempt to join upstream Wi-Fi yet.
- Change `SoftAP WPA2 password` to your own AP password, using 8–63 ASCII characters.
- You can keep the AP name `ESP32-Transparent-Proxy`.
- Keep other hardware/network defaults initially. Normal use does not require `Temporary gateway diagnostics`.

Save, exit the menu, and run:

```text
idf.py build
idf.py size
```

`build` generates firmware; `size` reports its memory footprint and does not flash the board. The first build downloads components and takes longer than subsequent builds.

`sdkconfig.defaults` supplies initial ESP32-S3, Flash, PSRAM, and feature settings. It does not overwrite an existing `sdkconfig`, so check the board settings when reusing an older project configuration.

Patch preparation, build, and size checks have been verified on Linux x86_64. Windows 11/EIM, distribution package installation, and WSL2 USB steps were checked against official instructions and still require validation on those systems and a physical board.

## 4. Flash the source-built firmware

On Windows 11, compare **Device Manager → Ports (COM & LPT)** before/after connecting the board to find its COM port; install the vendor driver for its USB bridge when needed. On Linux, compare the serial device list and inspect permissions:

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
ls -l /dev/ttyACM0
```

`/dev/ttyACM0` and `COM5` are examples; substitute the actual port. macOS commonly uses `/dev/cu.*`. If the Linux device belongs to `dialout`, use `sudo usermod -aG dialout "$USER"`, then log out/in; handle other groups according to actual ownership, rather than routinely flashing as root.

Linux, from the activated project terminal:

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

A Windows activated PowerShell project terminal:

```powershell
idf.py -p COM5 flash monitor
```

`flash` writes the firmware, and `monitor` opens logs at **115200 baud**. Press `Ctrl+]` to exit ESP-IDF monitor. Flashing, monitoring, and other serial tools must not use the same device simultaneously.

The current configuration uses a **UART0 console**. Prefer the board's UART / USB-UART connector; being able to flash over native USB does not mean that connector displays this firmware's UART logs. If automatic download-mode entry fails, use BOOT/EN according to the board's instructions.

**WSL2 USB:** install [usbipd-win following Microsoft's instructions](https://learn.microsoft.com/en-us/windows/wsl/connect-usb) and start the WSL2 distribution. These are Windows PowerShell commands. Replace example BUSID `4-4` with the device from `usbipd list`; `bind` requires administrator privileges:

```powershell
usbipd list
usbipd bind --busid 4-4
usbipd attach --wsl --busid 4-4
```

Then find `/dev/ttyACM*` / `/dev/ttyUSB*` in WSL Bash and flash using the Linux command. A reset may re-enumerate the device, requiring attachment to be checked again. Windows and WSL serial tools cannot use the same USB device simultaneously.

### Partitions, images, and reflashing

`partitions.csv` defines this layout. Remaining space on the 16 MB Flash is unallocated; there is no OTA or filesystem partition:

| Partition | Offset | Size | Purpose |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | `0x6000` (24 KiB) | Persistent configuration and other NVS data |
| `phy_init` | `0xf000` | `0x1000` (4 KiB) | Reserved PHY initialization data; current configuration has no separate PHY image |
| `factory` | `0x10000` | 4 MiB | Application |

A normal build generates `build/bootloader/bootloader.bin` (starts the application), `build/partition_table/partition-table.bin` (describes the layout), and `build/esp32-socks5-gateway.bin` (application). Current `build/flash_args` places them at `0x0`, `0x8000`, and `0x10000`; `idf.py flash` uses those build arguments, so offsets need not be guessed.

The `idf.py flash` command above erases/writes the three separate image ranges; it is not `erase-flash`. With the same layout, existing NVS settings normally remain. A full erase clears network/management credentials. The precompiled merged `factory.bin` also clears NVS because its padding covers that region, even without a full erase; the compatible `app.bin` update writes only the application. See the [precompiled flashing guide](flash-prebuilt.md). Do not erase routinely for upgrades; save settings before changing partition layouts or incompatible configuration formats. Existing NVS settings take precedence over new compiled defaults.

An exception is startup recovery from `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`: the current code erases NVS and retries, so settings can be lost. If startup fails before the hardware/NVS checks complete, serial recovery commands are not yet available; inspect boot logs first. See [recovery limits](maintenance.md#tested-boundary).

## Build troubleshooting

| Symptom | Check first |
| --- | --- |
| `idf.py` not found | Activate ESP-IDF in this terminal, then enter the project directory |
| Wrong Python / CMake / Ninja version or path | Python ≥ 3.10, CMake ≥ 3.22, and the selected SDK's terminal; do not mix Windows and WSL toolchains |
| lwIP revision/hash mismatch | Check both pinned commits and complete submodules. LF/CRLF alone is accepted; inspect actual edits with `git diff` in the SDK's lwIP repository, or back up the project override before recreating it. Do not force the patch |
| Build still refers to another SDK or global lwIP | Select the correct environment and run `idf.py reconfigure`; if stale cache remains, run `idf.py fullclean` inside the project and rebuild |
| Component download fails during build | Read the first download error; check access to GitHub, Espressif downloads, and the component registry; keep TLS/hash verification enabled |
| Serial port missing or flashing fails | Data cable, board USB connector, driver, port permissions, WSL USB attachment, and other programs using the port |
