# 无需 ESP-IDF，刷入预编译固件

[English](flash-prebuilt.md) · [简体中文](flash-prebuilt.zh-CN.md) · [日本語](flash-prebuilt.ja.md)

Windows 11 或 Linux 用户可以使用乐鑫官方工具为 **ESP32-S3 N16R8** 刷写固件，无需 Git、CMake、Ninja、ESP-IDF、交叉编译器，也无需运行项目自带的刷写脚本。Windows 还有不需要 Python 的官方图形工具。需要修改编译默认值时，再按[源码编译与刷写指南](build-and-flash.zh-CN.md)操作。

## 1. 选择合适的镜像

正式[项目 Release](https://github.com/noaic/esp32-socks5-gateway/releases)使用 `vYY.MM.DD` 标签，仅提供两个固件 BIN，命名为 `esp32-socks5-gateway-vYY.MM.DD-esp32s3-n16r8-{factory,sysupgrade}.bin`。Actions 仍提供完整开发验证包，但不会自动发布 Release。尚无 Release 时可从源码构建。

| 文件 | 用途 |
| --- | --- |
| `*-factory.bin` | 首次安装 / 重置用完整镜像：Bootloader + 分区表 + 应用。写入 **`0x0`**，**会重置已保存的网络和管理配置** |
| `*-sysupgrade.bin` | 应用更新镜像。写入 **`0x10000`**；要求板上已有兼容的 Bootloader、分区布局和配置版本 |

完整镜像由编译产物合并生成，**不是已配置设备的 16 MB Flash 转储**，也不会填充到整个 Flash 容量。但各镜像之间的空隙会填入 `0xFF`，包括 NVS 所在的 `0x9000–0xefff`：即使没有执行 `erase-flash`，刷入 `*-factory.bin` 也会覆盖这些已保存的配置。需要保留配置的兼容更新应选择 `*-sysupgrade.bin`；空白开发板不能只刷应用镜像。

**硬件要求：**ESP32-S3、恰好 16 MB Flash 和 8 MB Octal PSRAM、支持数据传输的 USB 线及稳定供电。镜像采用 DIO / 80 MHz Flash 配置，固件启动时检查 PSRAM。默认 LED 是 GPIO48 上的 WS2812，不适配时可在管理页禁用。文件名或芯片识别不能判断开发板的所有接线差异。

以下命令以 `v26.10.16` 为例，请将固件名称前缀改为实际下载版本。SHA-256 与发布页或 GitHub 附件摘要比较。许可证和上游声明包含在 Release 自动生成的源码包中，见[第三方声明](../THIRD_PARTY_NOTICES.md)。这里的 `sysupgrade` 是仅更新应用的**串口刷写**，固件没有实现 OTA 或 OpenWrt 的 `sysupgrade` 命令。

## 2. Windows 11：PowerShell

### 安装官方 esptool 并校验镜像

必要时从 [python.org](https://www.python.org/downloads/windows/) 安装 **Python ≥ 3.10**。使用普通 PowerShell 即可，不需要 ESP-IDF 终端。如果安装后提供的是 `py` 命令，可将开头的 `python` 改为 `py -3`。

将已下载的 Release BIN 复制到新目录，再创建独立工具环境：

```powershell
$Firmware = 'esp32-socks5-gateway-v26.10.16-esp32s3-n16r8'
New-Item -ItemType Directory -Force C:\esp\gateway-firmware | Out-Null
Copy-Item -Path "$env:USERPROFILE\Downloads\${Firmware}-*.bin" -Destination C:\esp\gateway-firmware
Set-Location C:\esp\gateway-firmware
Get-FileHash -Algorithm SHA256 "${Firmware}-factory.bin"
python --version
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install esptool==5.5.0
```

将 SHA-256 与发布页比较，忽略字母大小写；更新时改为校验 `-sysupgrade.bin`。直接调用虚拟环境中的 Python，无需调整 PowerShell 执行策略。这里使用官方 esptool 模块。

### 识别串口并刷写

连接开发板，对照连接前后的 **设备管理器 → 端口（COM 和 LPT）**；必要时安装 USB 转串口芯片厂商的驱动。将下文 `COM5` 换成实际串口，关闭占用串口的其他软件。先确认识别出的 Flash 为 **16 MB**：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 flash-id
```

**首次安装，或确定要重置配置**时：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 "${Firmware}-factory.bin"
```

如果是**已有兼容 Bootloader / 分区 / 配置版本的应用更新**，改用下面这一条：

```powershell
.\.venv\Scripts\python.exe -m esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x10000 "${Firmware}-sysupgrade.bin"
```

无需额外擦除整个 Flash。刷写成功后，可选打开 UART0 控制台：

```powershell
.\.venv\Scripts\python.exe -m serial.tools.miniterm COM5 115200 --raw
```

按 `Ctrl+]` 退出。原生 USB 刷写口可能与 UART / USB-UART 日志口不同。

### 可选：不安装 Python，使用官方图形工具

下载乐鑫 [Flash Download Tool](https://docs.espressif.com/projects/esp-test-tools/en/latest/esp32s3/production_stage/tools/flash_download_tool.html)。选择 **ESP32-S3**、开发板对应的下载连接方式和 COM 口，添加 `*-factory.bin`，地址填 **`0x0`** 并勾选该行。保持 **DoNotChgBin** 启用，以保留镜像自带的 DIO / 80 MHz / 16 MB 参数，先用 115200 波特率开始。该首次安装镜像会重置配置；兼容的应用更新则只选择 `*-sysupgrade.bin`，地址填 `0x10000`。普通更新不需要点击擦除功能。

## 3. Linux：Bash

Ubuntu / Debian：

```bash
sudo apt-get update
sudo apt-get install python3 python3-venv
```

Fedora（Python 包含 `venv`）：

```bash
sudo dnf install python3 python3-pip
```

使用 Python ≥ 3.10。复制 Release BIN，核对哈希后安装官方工具：

```bash
firmware_prefix='esp32-socks5-gateway-v26.10.16-esp32s3-n16r8'
mkdir -p ~/esp/gateway-firmware
cp ~/Downloads/"${firmware_prefix}"-*.bin ~/esp/gateway-firmware/
cd ~/esp/gateway-firmware
sha256sum "${firmware_prefix}-factory.bin"
python3 --version
python3 -m venv .venv
.venv/bin/python -m pip install esptool==5.5.0
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
```

将哈希与发布页中对应文件名的值或 GitHub 附件摘要比较；更新时校验 `${firmware_prefix}-sysupgrade.bin`。校验值能检测损坏，但仍应使用可信来源。

把 `/dev/ttyACM0` 换成实际设备。用 `ls -l /dev/ttyACM0` 查看所属组；如果属于 `dialout`，执行 `sudo usermod -aG dialout "$USER"` 后重新登录。其他组按发行版规则处理，不建议日常以 root 刷写。先确认 Flash 识别为 **16 MB**：

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 flash-id
```

**首次安装 / 重置配置**：

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x0 "${firmware_prefix}-factory.bin"
```

**兼容的应用更新**则改用：

```bash
.venv/bin/python -m esptool --chip esp32s3 --port /dev/ttyACM0 --baud 115200 write-flash 0x10000 "${firmware_prefix}-sysupgrade.bin"
```

可选 UART0 控制台：

```bash
.venv/bin/python -m serial.tools.miniterm /dev/ttyACM0 115200 --raw
```

按 `Ctrl+]` 退出。**WSL2：**Windows COM 口不会自动变成 Linux 串口，需按 [WSL2 USB 映射步骤](build-and-flash.zh-CN.md#4-刷写自己编译的固件)连接，再使用 `/dev/tty*`。直接从 Windows 刷写可省去这一步。

## 4. 首次联网、更新与排错

首次安装后，默认 AP 为 `ESP32-Transparent-Proxy`，密码为 `esp32proxy`，管理地址是 **http://192.168.4.1/**，上游 Wi-Fi 尚未配置。设置管理口令、Wi-Fi / SOCKS5 参数，正常使用前更换共享的默认 AP 密码。按[首次联网步骤](../README.zh-CN.md#烧录并完成首次联网)操作，保存网络设置后需在 **180 秒内确认**。

仅更新应用通常保留 NVS，已保存的配置优先于编译默认值。更新前先看版本兼容说明：分区变化、配置不兼容或固件的 NVS 恢复逻辑仍可能要求重新配置 / 清除配置。当前布局下 `*-factory.bin` 始终覆盖 NVS 区域，选择重置前应记下自己的设置；固件文件不是配置备份。参见[分区与重新刷写](build-and-flash.zh-CN.md#分区镜像与重复刷写)。

先用 115200 波特率，可靠连接后可选 `--baud 460800`。连接失败时检查数据线、驱动、端口、USB 接口、权限和串口占用；不能自动进入下载模式时按开发板 BOOT / EN 说明操作。不要忽略芯片 / Flash / 分区不匹配并强制刷入。

### 进阶：分段镜像和可选辅助脚本

Actions 和本地开发包仍保留 `firmware.zip`、`manifest.json`、`SHA256SUMS`、版权材料和三份原始镜像，以便验证。包内 `factory.bin` 对应正式 `-factory.bin`，`app.bin` 对应 `-sysupgrade.bin`；这些辅助文件不作为正式 Release 附件。分区兼容时，可用官方 esptool 分段更新三份镜像，避开 NVS 空隙：

```text
esptool --chip esp32s3 --port COM5 --baud 115200 write-flash 0x0 images/bootloader.bin 0x8000 images/partition-table.bin 0x10000 images/esp32-socks5-gateway.bin
```

若 PATH 中没有 `esptool` 命令，改为前文的虚拟环境 Python 加 `-m esptool`。项目可选辅助脚本仍放在源码仓库的 `tools/flash_firmware.py`，用于检查固件包并写入这三个分段镜像；**下载包不含该脚本，普通刷写也不需要它**。参见[工具说明](../tools/README.md)。

## 维护者生成固件包

先完成 SDK 和 lwIP 准备，再按[独立公开配置构建](build-and-flash.zh-CN.md#public-release-build)操作。源码指南中建议修改 AP 密码的私人构建会被公开打包工具有意拒绝；不要为打包而重置或覆盖私人配置。在已激活的项目终端执行：

```text
python tools/package_firmware.py --build-dir build-release --output-dir dist/gateway-validation
```

这会生成完整开发/CI 校验包。正式 Release 应从**干净的 `vYY.MM.DD` 标签**构建，改用带可选导出目录的命令：

```text
python tools/package_firmware.py --build-dir build-release --output-dir dist/gateway-validation --release-dir dist/gateway-release
```

两个输出目录都必须尚不存在、彼此不同且互不嵌套。第二条命令已同时创建两份输出，不要复用第一条例子刚生成的目录。工具从已编译的应用读取版本、核对干净源码 / 标签来源，恰好导出两个规范命名的 `-factory.bin` 与 `-sysupgrade.bin`，无需手工指定文件名或覆盖版本。完整包留作验证，哈希写入英文发布说明，版权材料链接到对应源码包中的声明。不要分发个人口令、设备 NVS 转储或日志。命令不会发布到远端，见[工具说明](../tools/README.md)。

## 验证范围

打包、完整性 / 失败处理、命令生成有主机测试与 Linux 构建验证；官方 esptool 可在无硬件时检查生成的镜像。Windows / WSL2 与图形工具步骤依据官方接口编写，本云环境没有开发板，不能验证 Windows 实际刷写或 ESP32-S3 启动。无线吞吐仍参考[性能与验证](performance.md)中的历史测量。
