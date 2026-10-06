# web41：个人使用的 AP 本地 Web 管理

2026-10-05。历史 web41 候选，已由 [web42 首次设置口令](web42.md) 替代；下文串口初始密码方式不适用于当前代码。已完成本机编译与回归测试，未烧录；板上保留 multi40。不增加 BLE、OTA、用户账号、认证门户、计费、黑名单或限速。

## 文件与架构

新增：

- `main/gateway_settings.h/.c`：固定大小、带版本的 NVS 配置记录和启动时不可变配置；保存、试运行、确认、回退。
- `main/settings_validation.c`：SSID/密码字节长度、IPv4、端口及最大接入数校验。
- `main/management_request.h/.c`：有界 JSON 解析和配置补丁规则。
- `main/web_management.h/.c`：AP-only HTTP、管理鉴权、状态/配置/断开接口和重启确认计时。
- `main/management.html`：Flash 内嵌、无外部依赖的移动端页面。
- `tests/test_gateway_settings.py`：运行真实校验及配置存储代码，NVS/随机数接口使用本机桩；模拟保存失败和试运行回退。

修改：`main/app_main.c`、`main/proxy_config.h`、`main/CMakeLists.txt`、`main/net_diagnostics.h/.c`、`README.md`。

Wi-Fi 和代理读取本次启动固定的配置；HTTP 保存的是下一次启动的配置，不在线修改正在转发的目标、socket 或 flow。断开设备通过 SDK Wi-Fi API 完成，随后由原有 Wi-Fi 事件和 token 校验处理连接清理。没有新转发任务，也不修改 radio、TCP window、总 flow 容量或 lwIP patch。

管理 HTTP 为一个低优先级任务，优先级 3、栈 6144 字节，最多 2 个 HTTP 客户端，超时 3 秒，绑定 AP netif 并核对本地 IP 与客户端子网。没有 STA 管理入口。HTTP 服务自身另外使用 3 个内部 socket（一个 TCP 监听和两个 UDP 控制 socket）；这些也占 lwIP 资源，不代表预留了 TCP PCB 容量。

状态页前台每 10 秒刷新、后台暂停。状态读取原有逐客户端统计及原子诊断计数，不遍历 TCP PCB、不新增主动 SOCKS/DNS 探测。客户端流量是本次 association 的代理有效载荷统计，断开重连后重新计数；不代表网卡总流量。HTTP 任务最低剩余栈单位为 ESP-IDF 的字节。

## 使用

连接 AP 后打开 **http://192.168.4.1/**。用户名固定为 `admin`。

首次创建配置时生成 24 位十六进制随机管理密码并保存 NVS；在没有修改这个初始密码之前，每次启动都会打印登录信息，避免错过首次日志。随机数熵源只在 RF/ADC 初始化前暂时开启。设置自选管理密码并确认后，启动日志不再打印密码。Wi-Fi 密码与管理密码不从任何 API 返回。

- 页面显示 AP 客户端、STA IPv4/RSSI、上游配置、内部 RAM 最低值/最大连续块、PSRAM、TCP PCB 及发送错误和重传计数。
- 可以修改 Wi-Fi、SOCKS5 IPv4/端口、DHCP DNS、最大接入数 1～2，以及管理密码。
- 密码框留空表示保留；上游无密码网络必须明确勾选“无密码”。清空 STA SSID 只启动 AP、不尝试连接上游。
- SSID 上限 32 **字节**；AP 密码 8～63 字节；STA 密码为空、8～63 字节或 64 位十六进制 PSK；管理密码 12～64 字节。浏览器字符限制之外，设备端会再次校验字节长度。
- 上游/DNS 禁止零地址、回环、组播及 AP 本地 `192.168.4.0/24`，只接受四段十进制 IPv4，不接受域名或有歧义的前导零。
- “断开”核对当前 token 与 SDK 的 AID；设备可能立即自动重连，不是永久禁用。尚未分配 IP 的连接需等待 DHCP 后操作。断开自己的设备可能收不到成功回复。
- “恢复默认”恢复当前固件编译时 Wi-Fi 配置和默认 SOCKS/DNS/两客户端设置，保留管理密码；不会清除整个 NVS 分区。

HTTP Basic 只用于管理权限隔离，不提供独立的传输加密；适合当前可信的个人 WLAN。修改接口要求 JSON、专用请求头，并拒绝跨来源请求。未实现 HTTPS 或联网账号。NVS 使用当前未加密分区，保存 Wi-Fi 和管理凭据；固件/sdkconfig、快照及初始串口日志应按包含个人凭据的文件保管。

## 保存与恢复

单个版本化 NVS blob 同时保存 active/previous，避免分开更新两个配置键。状态：

1. 保存候选配置，标记 staged；约 2 秒后重启。
2. 下次启动尝试候选配置，持久化 attempted 标记。AP 管理服务启动后有 180 秒确认窗口。
3. 重连 AP、打开页面，先验证上网，再点击“确认保留配置”；提交成功后关闭确认计时器。
4. 超时自动恢复 previous 并重启；若在确认前复位、断电或崩溃，下次启动直接恢复 previous，不反复尝试失败配置。候选版 HTTP 启动失败也触发重启恢复。

计时操作排入 HTTP 服务任务，与配置保存串行；普通服务没有常驻轮询计时器。NVS 操作和重启仅发生在配置操作或回退时。超时会受正在处理的有界 HTTP 请求延后，不是硬实时控制。

修改 AP 名称/密码后需要连接新 AP；修改管理密码后需要新密码，必要时重新打开页面/无痕窗口，避免浏览器复用旧 Basic 凭据。仅能访问页面并不表示 SOCKS 出口正常，所以不会自动替用户确认网络配置。

menuconfig Wi-Fi 值在首次没有有效 NVS 记录或恢复默认时使用；已有有效记录不会被重新编译的默认值覆盖。当前没有已忘记自选管理密码时的在线免认证恢复入口，需要离线维护 NVS；不要随意清除全 Flash。

## 构建和本机测试

```bash
cd /home/user/esp/esp32-transparent-proxy
source /home/user/.espressif/tools/activate_idf_v6.1.sh
idf.py build
python3 -m unittest discover -s tests -v
```

测试需要本机 socket 权限，沙箱内禁止 socket 时须在允许的本机环境运行。

- 原有 50 项测试通过，包括透明 rewrite/checksum、客户端生命周期/配额、TCP 半关闭与重连隔离、UDP/DNS、诊断包装和双终端测速同步。
- 新增 2 项测试通过：JSON/配置边界（含重复字段、嵌套、NUL、错误类型和前导零 IPv4）；模拟 NVS 提交失败、配置只在下一次启动生效、确认保留、复位/超时调用回退。
- 页面 JavaScript 语法检查通过。没有执行板上 HTTP/浏览器功能验收，不能由本机测试推出 Wi-Fi、实际 NVS 掉电或资源高水位已验收。

最终全量回归为 52 项全部通过（23.972 秒）。固件 876288 字节，相较 multi40 增加 65376 字节，4 MiB app 分区仍剩约 79%。SHA-256：`e4b03397acf77380a2eafaeda6d3a0695874ed1746cd6ddeb30c843b750a0443`。

记录：`build-web-management.log`、`test-web-management-all.log`（最终 52 项）、`test-web-settings.log`、`size-web-management.txt`。固件和源码快照：`build-snapshots/web41-management/`。

## 资源与待验收

相较 multi40，最终链接的静态 DRAM `.bss` 增加 1104 字节，IRAM text 增加 76 字节，另有对齐变化。HTTP 动态栈 6144 字节不包含在这 1104 字节中；server/session/请求解析/cJSON/NVS 还有运行时分配。页面约 8.5 KiB，嵌入 Flash，不申请测速 payload 缓冲。ELF dummy 区不应当作为应用变量的 PSRAM 分配量相加。

启动日志输出 HTTP 服务前后的内部 RAM 差值；此值受同时启动的 Wi-Fi/任务影响，只是采样，需结合页面的最低 RAM、最大连续块和栈余量评估。

烧录后依次验收：

1. 首次串口登录信息、页面鉴权、API 不泄露密码，确认服务未向 STA 开放。
2. 只读状态与两台实际客户端统计一致；B 断开/重连不影响 A 的数据传输。
3. 改成相同可用网络的小变化（例如 AP 名称），重连确认后再复位验证保留；另一次不确认，验证超时和手动复位恢复。
4. 管理密码修改/回退、恢复默认，DNS 修改后客户端重新获取 DHCP。
5. 两台上网、页面关闭/一台打开/两台打开三种状态，通过原测速服务自动上报比较吞吐与 RAM/PCB。测速地址保持 `http://192.168.1.10:18080/bench`，烧录本候选时应将后端采集标签更新到 web41，避免混入 multi40 结果。

近期应用记录曾达到 TCP PCB 上限，且内部 RAM 历史最低约 58 KiB。最多两个管理连接并不保证在所有转发资源满员时还能接入管理；本阶段先测量实际开销，不盲目扩大 PCB 或 socket 限额。未烧录，不提供硬件吞吐/内存开销的预测值作为实测结果。
