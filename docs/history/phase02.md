# Phase 2：IPv4 TCP 透明 local echo PoC

本阶段不接 SOCKS5。用户提供的 Phase 3 上游 `192.168.1.2:7897`
记录在 `main/proxy_config.h`，当前没有连接该地址的代码。

## 范围与架构

APSTA 和 DHCP 保持 Phase 1，GPIO48 保留。默认只捕获 AP 入站的 TCP 流量，
目的 IPv4 为 `198.51.100.7`，目的 TCP port 任意。测试地址可在 menuconfig
的 `Transparent TCP echo PoC` 菜单修改为其他非 AP 子网的单播 IPv4。
限定目的 IP 是为了避免 Windows 后台联网流量干扰最小 PoC。

示例映射：

```text
192.168.4.2:50000 -> 198.51.100.7:443
                   rewrite destination
192.168.4.2:50000 -> 192.168.4.1:15000 -> select echo

192.168.4.1:15000 -> 192.168.4.2:50000
                   rewrite source
198.51.100.7:443  -> 192.168.4.2:50000
```

TCP SYN/ACK、sequence、重传、拥塞控制、FIN/RST 全部由 lwIP 处理。
proxy 只检查 header/checksum，保存映射并双向改地址/port/checksum。
没有给 PCB 增加字段。`main/packet_rewrite.c` 是独立的可在主机测试的报文处理代码。

`main/tcp_poc.c` 使用静态 32 项 metadata 表，最多一个 active echo socket；
这不表示已完成 32 活跃连接代理。一个 4096-byte task 用 nonblocking sockets/select，
静态 2048-byte 待发送 buffer，对短读/短写提供 backpressure。
60 秒 socket idle timeout。关闭 socket 后的映射至少保留 2*MSL，并要求最近报文
也已静默 2*MSL；当前 MSL=60000 ms，即保留至少 120 秒，用于迟到 ACK/FIN 重传。
短时间连续测试最多保留 32 项；满表时丢弃新 SYN，过期后可复用。
同 client IP:port 连接到不同原始远端时会拒绝碰撞，不能坍缩成相同 lwIP tuple。
多 flow 的本地端口分配留给 Phase 3，不把这条限制隐藏成多连接支持。

表只在短的 portMUX critical section 中读写；分配、日志、socket、netif 输出都在锁外。
IPv4 hooks 只由 TCPIP context 调用，启用 `LWIP_CHECK_THREAD_SAFETY` 检查。

输入只在 IPv4 长度/checksum 校验后、目的地址复制和本机接收判定之前改写。
这样 `ip_data` 自动使用改写后的地址。匹配目标的 IP 分片（MF/offset）、IPv4 options、
原 TCP checksum 损坏、非法 header/长度会丢弃；不处理 UDP。
DHCP、ARP、ping AP 和非测试目的地址保持原有路径；没有 IP forwarding/NAPT。

输入先 clone 为连续可写 RAM，校验原报文后才创建 mapping。
输出 hook 在 IPv4 header/checksum 完成后、fragment/netif output 前：
只有命中的 AP 回包才用 `PBUF_LINK/PBUF_RAM` clone 改写和输出。
原 TCP pbuf 不被修改；ARP 排队可以持有 clone 的引用，返回后释放本方引用。
因此不污染 lwIP 重传队列。transient clone 分配失败会丢包或返回 ERR_MEM；TCP 重试
仍由 lwIP 处理。输出大于 MTU 时拒绝，本阶段不支持 IPv4 fragmentation。

## 可追踪的 lwIP patch

全局 ESP-IDF 不修改。`components/lwip/` 是生成的项目本地 component override，
源码与 manifest 固定在 v6.1 / esp-lwIP commit，项目只保存 patch，不提交整份 SDK。
`patches/lwip-v6.1-transparent-tcp.patch` 仅为 `ip4.c` 增加三个位置：
hook 声明、input 调用、output 调用。patch 有正反 `git apply --check` 检查。
脚本同时验证 revision、原始/修改后 SHA256、其他 component 文件完全一致。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
cd /home/user/esp/esp32-transparent-proxy
python tools/prepare_lwip.py apply
python tools/prepare_lwip.py check
idf.py reconfigure build
python tests/test_packet_rewrite.py
```

重复 apply/check 安全。恢复和重新应用：

```bash
# 先在补丁仍应用时关闭 PROXY_POC_ENABLE，再恢复。
idf.py menuconfig
python tools/prepare_lwip.py restore
# 此时 ip4.c 为原始版；可重复 restore。
idf.py build
# 恢复 PoC：先重新应用，再进入 menuconfig 打开 PROXY_POC_ENABLE。
python tools/prepare_lwip.py apply
python tools/prepare_lwip.py check
idf.py menuconfig
idf.py build
```

代码版本/内容不匹配时脚本停止，避免覆盖未知修改。
新增 SDK 版本需要重新审阅并更新 patch/manifest，不能直接套用旧 diff。
官方 API/线程安全参考：[IDF v6.1 lwIP 文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/lwip.html)，
socket event loop 参考本机 `examples/protocols/sockets/non_blocking`。

## Windows Ncat 快速验收（无脚本）

在连接 ESP32 AP 的测试 Windows 上，用 CMD 或 PowerShell 执行：

```text
echo hello123 | ncat -4 -v -w 5s -i 5s 198.51.100.7 443
```

应输出 Connected、收到的 hello123、发送/接收字节数，并正常退出。
stdin EOF 默认导致 Ncat 关闭发送方向，适合本 PoC 检查 client FIN / server EOF。
不要添加 SSL、代理、send-only 或 no-shutdown 参数。目标仍为假的远端 IP/port，
不是 ESP32 网关或 local listener；无需安装/运行 PowerShell 脚本。
参考 [Ncat 官方用法](https://nmap.org/ncat/guide/ncat-usage.html)。

## Android 图形界面快速验收

无需脚本。可使用 Google Play 上的
[TCP Client（HARDCODED JOY）](https://play.google.com/store/apps/details?id=com.hardcodedjoy.tcpclient)，
应用支持指定 TCP IP/port、文本/hex 收发和 RX/TX 计数；商店显示 2026-04-18 更新。

先在仍有互联网时安装。断开原 Windows 测试终端与 ESP32 AP 的连接，
确保唯一客户端名额空出；开发电脑保持原网络。Android 连接
`ESP32-Transparent-Proxy` / `esp32proxy`，选择保留“无互联网”的 Wi-Fi。
测试时关闭手机移动数据/VPN，避免测试 socket 绕过 AP。

选择 TCP client，目标 `198.51.100.7`，port `443`，发送 `hello123`。
必须在 RX 接收区域看到同样内容，不能把发送记录或 local echo 当成远端回显。
再发送 `abc` 和 `0123456789`，之后主动 Disconnect。
用户反馈是否连接、收到哪些内容；设备串口核对 mapping、SYN/FIN、EOF 和关闭。
该快速测试证明基本小 payload echo，不能替代 binary/大 payload、重传和长期压测。

## Windows 客户端验收

测试从实际连接 AP 的终端执行。开发电脑继续连接上游网络，负责构建/串口即可，
不要求开发电脑切换到 ESP32 AP。若 AP 客户端是另一台终端，请在那台终端上测试；
WSL 通过开发主机上游网络发出的包不经过 AP 入站路径，不能替代透明代理验收。

带 Python 3 的 AP 客户端可使用跨平台、无第三方依赖脚本：

```bash
python3 test_transparent_echo.py
```

源码在 `tools/test_transparent_echo.py`。传到客户端后运行；不要在未连接 AP 的
开发 WSL 中运行并误认为已验证 AP 透明路径。Android/iOS 方法依赖客户端已有的
终端工具，先确认实际平台再选择，不假定需要改变开发电脑网络或安装新应用。

连接 AP `ESP32-Transparent-Proxy` / `esp32proxy`，确认客户端地址。
如同时保留以太网或其他默认路由，以管理员 PowerShell 为测试 IP 添加临时 host route：

```powershell
route add 198.51.100.7 mask 255.255.255.255 192.168.4.1
# 完成后恢复：route delete 198.51.100.7
```

运行项目提供的 PowerShell 脚本（自动检测 Windows AP 网卡 IPv4，target 198.51.100.7:443）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\test-transparent-echo.ps1
```

若 Windows 默认 WSL 发行版就是当前 Fedora，直接读取并运行现有文件也可以：

```powershell
wsl.exe -e cat /home/user/esp/esp32-transparent-proxy/tools/test-transparent-echo.ps1 | Out-String | Invoke-Expression
```

也可从脚本文件运行时传入 `-ClientIP 192.168.4.x`。
脚本最初默认 .2，用户反馈 Bind 报“地址无效”，说明 Windows 当时没有该本机地址；
现已改为自动检测，缺少 AP 地址时会明确显示接口列表并停止，而不是尝试绑定错误地址。
测试脚本必须在实际 AP 客户端运行；WSL 仅保存源码，设备端 UART 日志不能替代
客户端对 endpoint、echo payload 和 FIN/EOF 的验证。
测试会验证 1/37/4096/16384-byte payload 和 64 轮 1024-byte binary payload，
累计 86,054 bytes，逐字节验证回显；打印 Local/Remote endpoint，
最后 shutdown(Send) 并要求 server FIN/EOF。成功输出 PASS。
浏览器不能用来测试 echo，因为本阶段没有 HTTP server/proxy。

设备侧采集（需要串口权限；启动新 capture 默认会 reset，一定在客户端测试前）：

```bash
python tools/uart_capture.py --seconds 180 --log monitor-phase02.log
# 已运行时附加观察，不主动 RTS 复位：
python tools/uart_capture.py --seconds 180 --no-reset --log monitor-phase02-followup.log
```

`--no-reset` 跳过脚本主动 RTS reset；串口适配器/驱动在打开或关闭设备时仍可能
改变控制线并触发硬件复位。本次重新打开采集曾观察到 ROM/boot 日志，
因此测试应在 capture 已打开、板子 ready 且客户端重新连接之后进行。

正常应记录 map SYN、output SYN（flags=0x12）、accept original destination、
input FIN、client EOF、output FIN、closed echo bytes_in/bytes_out 相同。
可用 Wireshark 在 Windows Wi-Fi 接口捕获 `ip.addr == 198.51.100.7 && tcp.port == 443`，
验证握手、FIN/ACK、回包源 IP/port 和 checksum；本地 15000 端口不应出现在客户端 wire 上。
若 NIC checksum offload 导致客户端发包显示 checksum 未验证/错误，应结合设备接收成功
和抓到的实际回包判断，不能仅凭出站抓包的 offload 标记断言固件 checksum 错误。

## 测试与风险

本阶段新增/修改文件清单：

| 用途 | 文件 |
| --- | --- |
| 报文处理与独立表 | `main/packet_rewrite.c`, `main/packet_rewrite.h`, `main/tcp_poc.c`, `main/tcp_poc.h` |
| 启动/配置 | `main/app_main.c`, `main/proxy_config.h`, `main/Kconfig.projbuild`, `sdkconfig.defaults` |
| 构建 | `CMakeLists.txt`, `main/CMakeLists.txt`, `.gitignore` |
| SDK patch | `patches/lwip-v6.1-transparent-tcp.patch`, `patches/lwip-v6.1.json`, `tools/prepare_lwip.py` |
| 测试 | `tests/test_packet_rewrite.py`, `tools/test-transparent-echo.ps1`, `tools/test_transparent_echo.py`, `tools/uart_capture.py` |
| 文档 | `README.md`, `docs/phase02.md`, `docs/phase01-validation.md` |

本地 `sdkconfig` 和 `components/lwip/` 为生成配置/副本，已忽略，凭据不进入源码。

本次构建 `idf.py build` 成功，无编译 warning/error，镜像 778,000 bytes。
`idf.py size` 和 image-info 成功。build/project_description.json 确认 lwIP
来自项目内 `components/lwip`。apply/check、重复 apply、restore、重复 restore、
重新 apply/check 均成功。全局 IDF 与 esp-lwIP git status 均保持 clean。
主机报文测试 4 项通过（含 300 个确定随机长度场景及边界样本）。
UART 烧录成功，hash verified；启动再次确认 PSRAM test OK 和 APSTA ready。
设备输出 `Phase 2 TCP echo ... local=15000; metadata=32 active_echo=1`，
ready 时 internal_free=270631 bytes，PSRAM_free=8385508 bytes。
客户端透明握手/echo/FIN 仍待实际测试结果，不以启动成功代替通过。

用户后续使用 Ncat 在另一台 Windows 客户端（192.168.4.4）测试，反馈：
Connected to 198.51.100.7:443、收到 hello123、10 bytes sent / 10 bytes received，
0.78 seconds。这确认客户端侧小 payload echo 成功；10 bytes 包含 Windows CRLF。
当时 900 秒 UART 采集已结束，没有覆盖该连接；设备 mapping/FIN 日志尚待重复测试补齐，
不能把旧启动日志当作本次连接日志。192.168.4.4 属于 AP 子网，固件不限制为 .2。

### Ncat 重复测试：基本路径通过

重新打开采集并复位后，DHCP 将同一 Windows 客户端重新分配为 192.168.4.2。
用户重复执行上述 Ncat 命令并确认收到回复。`monitor-phase02-ncat.log` 实际记录：

```text
map SYN 192.168.4.2:4082 -> 198.51.100.7:443 <-> 192.168.4.1:15000 flags=0x02
output SYN/FIN/RST ... flags=0x12
accept original destination 192.168.4.2:4082 -> 198.51.100.7:443
input FIN/RST ... flags=0x11
client EOF; drain echo and close
output SYN/FIN/RST ... flags=0x11
closed echo bytes_in=10 bytes_out=10; mapping retained for 2*MSL
heap internal_free=267895 largest=192512 PSRAM_free=8385644
```

客户端反馈与设备日志共同确认 SYN/SYN-ACK/ACK、原始目的地址恢复、小 payload 双向 echo、
FIN/EOF 正常关闭。未见 panic/watchdog/assert。Phase 2 最小基本路径验收通过。
源地址从 .4 变为 .2 是重新初始化 DHCP 后的实际分配，不需要修改固件。
本次不是大 payload/binary/丢包重传压测，也不证明长期无泄漏。

主机 tests 实际编译/调用 `packet_rewrite.c`，用独立 Python checksum 计算验证：
双向改写、payload/sequence/options 保留、SYN/FIN/RST、奇偶 payload、最大 IPv4 长度、
损坏报文、truncation、MF/offset、IP options、非法 TCP header。不替代板上的 pbuf/lwIP 验证。

当前尚不承诺吞吐、长期无泄漏、丢包重传稳定性、RST/timeout 全覆盖；
这些需要板上 traffic 和长时间压测。PoC 初次正常握手和 FIN 也不能证明全部 TCP 状态路径。
IPv4 packet clone 需要 transient heap；使用当前 internal/PSRAM 策略，Phase 5 再评估成本。
AP 跟随上游信道 13；不支持该信道的终端可能无法发现 AP。
