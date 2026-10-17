# 从源码编译与刷写

[English](build-and-flash.md) · [简体中文](build-and-flash.zh-CN.md) · [日本語](build-and-flash.ja.md)

本指南集中保留精确 SDK、lwIP 准备、配置、编译和刷写的完整步骤。普通用户无需安装 ESP-IDF，可选择[预编译固件刷写指南](flash-prebuilt.zh-CN.md)。刷写完成后返回[首次联网设置](../README.zh-CN.md#烧录并完成首次联网)。

## 开发依赖

### 软件

| 类别 | 依赖与用途 | 如何准备 |
| --- | --- | --- |
| 必需：开发环境 | Git、Python ≥ 3.10（建议 3.12）、CMake ≥ 3.22、Ninja | Windows 用官方 EIM 检查并安装；Linux 按下文安装系统包。当前 IDF 工具清单推荐 CMake 4.0.3、Ninja 1.12.1 |
| 必需：SDK | 指定提交的 ESP-IDF v6.1 与 esp-lwIP | 见下方提交表；ESP-IDF 中已包含 FreeRTOS 和 lwIP，不必单独安装它们 |
| 自动管理：工具链 | Xtensa GCC 15.2.0，生成 ESP32-S3 固件；以及 esptool、Python 构建工具和 Component Manager | EIM 或 ESP-IDF 安装脚本依据此提交的工具清单安装 |
| 自动管理：组件 | cJSON 1.7.19、led_strip 3.0.3 | `main/idf_component.yml` 声明，`dependencies.lock` 固定版本和哈希，首次构建下载到 `managed_components/` |
| 必需：使用环境 | 浏览器、已有 2.4 GHz Wi-Fi、支持 IPv4 / NO AUTH / UDP ASSOCIATE 的 SOCKS5 服务 | 用户自行准备；仅编译和宿主测试无需实际代理服务 |
| 可选：开发测试 | Linux 系统 GCC/binutils、Node.js；ccache 可加速重复构建 | GCC 用于宿主 C 测试，Node.js 用于前端测试，均不是固件中的运行依赖 |

管理页和首次设置页默认英文；浏览器首选语言为中文（`zh` 或 `zh-*`）时显示简体中文，其他语言或无法识别时使用英文。首选英文、备用中文时仍显示英文。串口命令为英文。电脑上的系统 GCC 与 ESP32 的交叉编译器用途不同；不要用前者替换固件工具链，也不要为通过构建而更新锁文件。

Windows 使用 **PowerShell**，Linux / WSL2 使用 **Bash**；请只执行对应系统的命令。SDK 和项目路径应避免空格及非 ASCII 字符，例如 Windows 的 `C:\esp`。以下步骤只在新目录中准备 SDK，不覆盖其他项目正在使用的 ESP-IDF。

## 1. 安装本项目对应的 ESP-IDF

本项目为透明转发使用了一个小型 lwIP 补丁。lwIP 是 ESP-IDF 内部处理 TCP/IP 的网络库；补丁依赖其具体源码，所以仅显示“v6.1”还不够，必须匹配下面的提交：

| 依赖 | 精确提交 |
| --- | --- |
| ESP-IDF v6.1 | `fff9895c82d744c7237be8847347bdd1b07c6643` |
| esp-lwIP | `c6f2f878e7b0f86033214b85547d579be43351e3` |

### Windows 11：官方 EIM 与 PowerShell

1. 从 [乐鑫官方页面](https://dl.espressif.com/dl/eim/) 下载 EIM 图形安装器。按 [Windows 官方安装说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/windows-setup.html)完成依赖检查；若 Git 尚未可用，先安装 [Git for Windows](https://git-scm.com/install/windows)，再打开新的 PowerShell。无需使用 `winget`。
2. 在普通 PowerShell 中将指定提交放入**尚不存在的** `C:\esp\esp-idf-gateway`。准备脚本兼容 LF 和 CRLF 检出，无需调整 Git 换行设置：

```powershell
git --version
New-Item -ItemType Directory -Force C:\esp | Out-Null
git clone --no-checkout --depth 1 https://github.com/espressif/esp-idf.git C:\esp\esp-idf-gateway
Set-Location C:\esp\esp-idf-gateway
git fetch --depth 1 origin fff9895c82d744c7237be8847347bdd1b07c6643
git checkout --detach fff9895c82d744c7237be8847347bdd1b07c6643
git submodule update --init --recursive --depth 1
```

3. 在 EIM 中选择 **Custom Installation**，将路径指向这个现有的 Git 目录。官方 [Expert Installation](https://docs.espressif.com/projects/idf-im-ui/en/latest/expert_installation.html)支持使用已有仓库而不改写其内容。安装该检出的工具链、CMake、Ninja 和 Python 环境；使用此目录，不要另外安装“最新版本”后直接用它构建。
4. 安装完成后，在 EIM 的 **Manage Installations → Open IDF Terminal** 中打开对应安装的 PowerShell，或使用它生成的 PowerShell 快捷方式。**后续 Windows 命令都在该已激活终端执行**，新开普通 PowerShell 不会自动带上环境：

```powershell
python --version
idf.py --version
git -C "$env:IDF_PATH" rev-parse HEAD
git -C "$env:IDF_PATH\components\lwip\lwip" rev-parse HEAD
git -C "$env:IDF_PATH\components\lwip\lwip" ls-files --eol src/core/ipv4/ip4.c
cmake --version
ninja --version
```

两个提交输出须与表中一致。换行命令仅用于诊断：`i/lf w/lf` 和 `i/lf w/crlf` 都受支持，后者表示 Git 检出了 Windows 换行，并不等于源码被修改，无需因此重装已有 SDK。EIM 使用其生成的激活入口；传统脚本安装在该 SDK 目录中，PowerShell 安装用 `.\install.ps1 esp32s3`，激活用 `.\export.ps1`，CMD 对应 `install.bat esp32s3` / `export.bat`。Windows 不使用 Bash 的 `source`。

### Linux：Ubuntu/Debian 或 Fedora，Bash

Ubuntu/Debian 使用 [官方系统包列表](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/linux-macos-setup-legacy.html)，建议选择提供 Python ≥ 3.10 的版本，例如 Ubuntu 22.04 或更新版本：

```bash
sudo apt-get update
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

Fedora 对应包名不同，使用 `dnf`；其 Python 包提供 `venv` 模块：

```bash
sudo dnf install git wget flex bison gperf python3 python3-pip cmake ninja-build ccache libffi-devel openssl-devel dfu-util libusb1
```

接下来使用独立的 SDK 目录；`esp-idf-gateway` 应尚不存在：

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

安装脚本下载匹配的编译器和 Python 依赖；另行请求 CMake/Ninja 可避免系统工具过旧。**每次打开新 Bash 都要重新激活**，然后检查实际版本：

```bash
source ~/esp/esp-idf-gateway/export.sh
idf.py --version
git -C "$IDF_PATH" rev-parse HEAD
git -C "$IDF_PATH/components/lwip/lwip" rev-parse HEAD
cmake --version
ninja --version
```

### 能否复用已有环境？

已安装的 SDK 只有在两个 Git 提交都匹配、子模块完整且全局 lwIP 源码未经修改时，才能直接复用。先通过它自己的激活入口运行上述检查；同样显示“v6.1”不代表同一提交。若不同，为本项目新建独立 SDK，不要在共享安装上直接 `git pull` 或切换版本。EIM 安装使用 EIM 的终端；脚本安装使用对应 `export` 脚本。

浅克隆时 `idf.py --version` 可能提示 `Git version unavailable`，随后读取源码版本；仍以两个精确提交和补丁脚本的校验为准。

WSL2 应在 Linux 发行版内执行 Linux 安装步骤，不能直接复用 Windows 编译器。USB 映射见下方刷写说明；macOS 用户可参考官方指南，Bash 的项目命令相同。

## 2. 获取项目并准备 lwIP

Windows 已激活的 PowerShell：

```powershell
git clone https://github.com/noaic/esp32-socks5-gateway.git C:\esp\esp32-socks5-gateway
Set-Location C:\esp\esp32-socks5-gateway
python tools/prepare_lwip.py apply
python tools/prepare_lwip.py check
```

Linux / WSL2 已激活的 Bash：

```bash
git clone https://github.com/noaic/esp32-socks5-gateway.git ~/esp32-socks5-gateway
cd ~/esp32-socks5-gateway
python3 tools/prepare_lwip.py apply
python3 tools/prepare_lwip.py check
```

已有项目目录时直接进入该目录，不必重复克隆。后续的项目命令都在**包含 `CMakeLists.txt` 的项目根目录**执行。

`patches/lwip-v6.1-transparent-tcp.patch` 保存 `ip4.c` 的改动，`patches/lwip-v6.1.json` 记录两个提交与修改前后的 SHA-256。脚本从 `$IDF_PATH/components/lwip` 复制组件及其 lwIP 子模块中由 Git 跟踪的文件到项目 `components/lwip`，再只修改该副本。ESP-IDF 的 [同名组件优先规则](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/build-system.html#multiple-components-with-the-same-name)会选用项目副本，因此全局 SDK 和其他项目不受影响；生成目录已被 Git 忽略。

校验 `ip4.c` 时，脚本在内存中将 CRLF 视为 LF，仍使用原有固定哈希。`apply`、`restore` 只将已验证的项目副本写为 LF；补丁本身为 CRLF 时也可使用，不会改写补丁文件。`check` 完全只读。`apply`/`check` 将**全局 lwIP 组件及子模块中每个 Git 跟踪文件与已提交内容比较**，即使 HEAD 正确也检查文件本身。SDK 中未跟踪的测试 / 编辑器缓存不会复制，也不会因此拒绝准备；项目副本则必须恰好包含这些跟踪文件，仅允许 `ip4.c` 的既定补丁。所有文本比较均接受 LF/CRLF 检出差异，二进制须逐字节一致；代码或空白修改、BOM、单独的 CR、缺失的跟踪文件、项目副本中的意外新增文件和符号链接仍会被拒绝。

以下命令两种终端都可执行（Linux 也可用 `python3`）：

| 命令 | 具体用途 |
| --- | --- |
| `python tools/prepare_lwip.py apply` | 检查版本和内容，创建或复用本地副本，将其中的 `ip4.c` 规范为 LF 并应用补丁；可重复执行 |
| `python tools/prepare_lwip.py check` | 只读检查提交、按 LF 计算的原始/补丁哈希及完整组件的文件列表和内容 |
| `python tools/prepare_lwip.py restore` | 将已验证的本地 `ip4.c` 反向还原为 LF，无需激活 SDK；保留本地组件目录，不自动关闭透明功能 |

`restore` 只验证本地 `ip4.c` 的补丁状态，用于离线还原，不是全 SDK/组件完整性检查。完整验证应使用 `apply` 和 `check`。

默认配置启用了透明模式，`restore` 后必须重新 `apply`、`check` 才能继续正常构建；`check` 会拒绝未打补丁的副本。若已有构建曾引用全局 lwIP，准备副本后运行 `idf.py reconfigure` 再构建。版本错误会列出预期与实际提交；哈希错误表示存在 LF/CRLF 之外的差异。先备份并检查 SDK 或本地副本的意外修改，或准备独立的干净 SDK，不要强行覆盖。`apply`、`check` 也会拒绝其他组件文件的变化，写入时拒绝通过本地符号链接修改其他位置。未来升级 ESP-IDF/esp-lwIP 可能改变 `ip4.c` 路径、钩子位置或接口，需要另行审查和适配补丁，不能只改提交或哈希来绕过检查。

## 3. 配置并编译

以下配置与构建命令在两种已激活终端中相同：

```text
idf.py menuconfig
```

打开 `Transparent gateway Wi-Fi`：

- 第一次可以保持上游 `SSID` 为空，稍后通过管理页面填写；此时 AP 会启动，但 ESP32 不尝试连接上游。
- 将 `SoftAP WPA2 password` 改为你自己的 AP 密码，使用 8～63 个 ASCII 字符。
- AP 名称可以保留 `ESP32-Transparent-Proxy`。
- 其他硬件和网络参数先保留默认值；日常使用无需开启 `Temporary gateway diagnostics`。

保存并退出菜单后执行：

```text
idf.py build
idf.py size
```

`build` 生成固件；`size` 查看固件占用，并不烧录。首次构建会下载组件，所需时间比后续构建长。

`sdkconfig.defaults` 提供 ESP32-S3、Flash 和 PSRAM 等初始配置；一旦已有 `sdkconfig`，defaults 不会自动覆盖它。因此，旧项目的配置也应检查是否匹配板型。

Linux x86_64 的补丁准备、编译和容量检查已验证；Windows 11/EIM、发行版包安装和 WSL2 USB 操作依据官方说明核对，仍需对应系统与实体开发板验证。

<a id="public-release-build"></a>

### 单独构建可公开分发的默认固件

自己使用的 `sdkconfig` 和 `build/` 可以保留；公开打包工具会有意拒绝自定义 AP 密码和上游凭据。分发固件时，请在已激活的项目终端中使用**独立、初次使用**的 `build-release/` 目录与配置。

Windows PowerShell：

```powershell
$ReleaseConfig = Join-Path (Get-Location) 'build-release\sdkconfig'
idf.py -B build-release -D "SDKCONFIG=$ReleaseConfig" build
idf.py -B build-release size
```

Linux/WSL2 Bash：

```bash
idf.py -B build-release -D SDKCONFIG="$PWD/build-release/sdkconfig" build
idf.py -B build-release size
```

新配置读取 `sdkconfig.defaults` 中的公开默认值：上游 Wi-Fi 为空、初始 AP 共享密码为 `esp32proxy`，刷写后首次设置时再更换 AP 密码。命令不会覆盖项目根目录的私人 `sdkconfig`。如果 `build-release/sdkconfig` 已存在，仍以它为准；先检查内容，或改用另一组未使用的构建 / 配置路径，不要复制私人配置。接下来见[生成固件包](flash-prebuilt.zh-CN.md#维护者生成固件包)。

## 4. 刷写自己编译的固件

Windows 11 在“设备管理器 → 端口（COM 和 LPT）”中比较插拔前后的设备，找到新增的 COM 端口；按板上 USB 桥类型安装厂商驱动。Linux 可比较插拔前后的串口列表，并检查权限：

```bash
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null
ls -l /dev/ttyACM0
```

`/dev/ttyACM0` 和 `COM5` 都只是示例，替换为实际端口；macOS 通常为 `/dev/cu.*`。若 Linux 串口所属组为 `dialout`，可执行 `sudo usermod -aG dialout "$USER"`，注销后重新登录；其他组按设备实际权限处理，不要靠长期使用 root 烧录。

Linux 已激活的项目终端：

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

Windows 已激活的 PowerShell 项目终端：

```powershell
idf.py -p COM5 flash monitor
```

`flash` 写入固件，`monitor` 打开串口日志，波特率为 **115200**。按 `Ctrl+]` 可退出 ESP-IDF monitor。烧录、monitor 和其他串口工具不要同时占用同一个设备。

当前配置使用 **UART0 控制台**，优先连接板上标为 UART / USB-UART 的接口；原生 USB 能烧录不代表该接口能显示本固件的 UART 日志。无法自动进入下载模式时，按板卡说明使用 BOOT/EN。

**WSL2 USB：**先在 Windows 安装 [usbipd-win，按微软说明操作](https://learn.microsoft.com/en-us/windows/wsl/connect-usb)，并启动 WSL2 发行版。以下是 Windows PowerShell 命令；`4-4` 是示例 BUSID，必须替换为 `usbipd list` 中的设备。`bind` 需要管理员权限：

```powershell
usbipd list
usbipd bind --busid 4-4
usbipd attach --wsl --busid 4-4
```

然后在 WSL Bash 中重新查找 `/dev/ttyACM*` / `/dev/ttyUSB*`，按 Linux 命令烧录。设备复位后可能重新枚举，需重新确认映射；同一 USB 设备不能同时供 Windows 和 WSL 的串口工具使用。

### 分区、镜像与重复刷写

`partitions.csv` 定义了以下布局；16 MB Flash 中剩余空间未分配，当前没有 OTA 或文件系统分区：

| 分区 | 起始地址 | 大小 | 用途 |
| --- | --- | --- | --- |
| `nvs` | `0x9000` | `0x6000`（24 KiB） | 持久配置等 NVS 数据 |
| `phy_init` | `0xf000` | `0x1000`（4 KiB） | PHY 初始化数据预留区；当前配置不生成单独的 PHY 镜像 |
| `factory` | `0x10000` | 4 MiB | 应用程序 |

正常构建生成 `build/bootloader/bootloader.bin`（启动应用）、`build/partition_table/partition-table.bin`（描述布局）和 `build/esp32-socks5-gateway.bin`（应用）。当前 `build/flash_args` 分别安排在 `0x0`、`0x8000`、`0x10000`；`idf.py flash` 会按构建参数处理，无需手工猜偏移。

上述 `idf.py flash` 按三份独立镜像擦写所需范围，不等于 `erase-flash`；分区布局一致时，现有 NVS 设置通常保留。擦除整片会清除网络与管理口令。预编译的完整 `factory.bin` 填充区也覆盖 NVS，因此即使不整片擦除，也会清除配置；兼容版本的 `app.bin` 更新则只写应用。见[预编译固件刷写指南](flash-prebuilt.zh-CN.md)。不要将擦除作为例行升级步骤；更换分区布局或不兼容的配置格式前应先保存设置。原有 NVS 配置优先于新固件的编译默认值。

例外是启动时遇到 `ESP_ERR_NVS_NO_FREE_PAGES` 或 `ESP_ERR_NVS_NEW_VERSION_FOUND`：当前代码会擦除 NVS 后重试，配置可能丢失。如果启动未通过硬件/NVS 检查，串口恢复命令尚未启动，应先查看开机日志。见 [恢复边界](maintenance.md#tested-boundary)。

## 常见构建问题

| 现象 | 优先检查 |
| --- | --- |
| `idf.py` 找不到 | 当前终端是否已执行 ESP-IDF 激活脚本；激活后再进入项目目录 |
| Python / CMake / Ninja 版本或路径错误 | 检查 Python ≥ 3.10、CMake ≥ 3.22，并使用对应 SDK 的终端；不要混用 Windows 与 WSL 工具链 |
| lwIP 提示 revision / hash 不匹配 | 检查两个固定提交及子模块是否完整。单纯 LF/CRLF 差异已兼容；在 SDK 的 lwIP 仓库中用 `git diff` 检查实际修改，或备份项目副本后再重新生成，不要强行打补丁 |
| 构建仍引用旧 SDK 或全局 lwIP | 确认正确环境并运行 `idf.py reconfigure`；缓存仍不一致时，在项目内 `idf.py fullclean` 后重新构建 |
| 编译无法下载组件 | 查看第一个下载错误，检查网络是否能访问 GitHub、乐鑫下载站和组件注册表；不要关闭 TLS / 哈希校验 |
| 找不到串口或无法烧录 | 数据线、板上 USB 接口、驱动、串口权限；WSL 中确认 USB 是否已转发；关闭其他占用串口的程序 |
