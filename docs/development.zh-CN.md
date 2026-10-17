# 实现原理与开发指南

[English](development.md) · [简体中文](development.zh-CN.md) · [日本語](development.ja.md)

先按[用户指南](../README.zh-CN.md)配置网关；需要自行编译时使用[源码编译指南](build-and-flash.zh-CN.md)。本页介绍实现原理、源码结构、宿主回归和可选工具。除特别说明外，命令均在项目根目录执行。

## 理解最基本的实现

`main/` 定义网关行为，使用 ESP-IDF 提供的 Wi-Fi、`esp_netif`、socket、HTTP 和 NVS API。**FreeRTOS 是 ESP-IDF 内置的调度系统**，负责 TCP/UDP、维护等任务及队列/锁；**lwIP 是其 TCP/IP 协议栈**，负责 IPv4、TCP 握手、重传等。应用决定如何转发，SDK 提供驱动与服务，FreeRTOS 调度执行，lwIP 处理网络协议；这些底层库不需要用户另装操作系统。

可以把 ESP32 看作接收和转交数据的中间人：

- **TCP** 常用于网页等需要可靠传输的连接。网关记住终端原本想连接的目标，再通过 SOCKS5 的 `CONNECT` 请求让代理连接它，最后把响应送回终端。
- **UDP** 常用于普通 DNS 查询和部分实时通信。网关通过 `UDP ASSOCIATE` 获取中继信息，再携带目标地址转发数据。不同通信保留各自的转发状态。
- **DNS** 把域名转换成 IP 地址。默认 DHCP 告诉终端使用 `1.1.1.1`；ESP32 没有本地 DNS 服务，普通 DNS UDP 请求也经 SOCKS5 转发。
- **NVS** 是 Flash 中保存配置的区域。因此设置通常能跨重启保留，重新烧录应用也不会自动覆盖已有的网络参数。

当前总容量为 64 个活动 TCP 转发连接、32 个 UDP flow；512 条目标/结束阶段映射另受每客户端槽位 256 条保留映射、16 条待监听端接纳映射的上限约束，**不表示可以同时转发 512 个 TCP 连接**。lwIP 负责 TCP 重传等底层工作，应用使用固定循环处理多条连接。

## 从一条网页连接看 TCP 转发

**为什么修改 `ip4.c`？** 手机的外部网站请求原本并不是发给 ESP32 本地地址，普通应用 socket 不能直接接收这些 TCP 连接。当前补丁在 IPv4 校验后、本地地址判断前接入输入钩子，让应用记录目标并改写；在 IPv4 输出头准备好、交给网络接口前接入输出钩子，恢复远端地址。改写使用独立的数据包副本，避免损坏 lwIP 保留用于重传的数据。仅建立一个 SOCKS5 socket，无法完成本项目所需的透明截获与地址恢复；也无需让应用重写整套 TCP 协议。参考 [ESP-IDF 的 lwIP 说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/lwip.html)。

假设手机想连接网站的 `203.0.113.10:443`，这里的地址只是说明用的示例：

1. 手机将数据包发给它的默认网关，也就是 ESP32。
2. 项目内的 lwIP 补丁在 IPv4 处理路径调用网关代码。代码检查数据包，记录手机和原始目标，将 TCP 数据包改写为送给 ESP32 本地监听端口，并更新校验和，让接收方能正确验证数据包。
3. lwIP 处理这段本地 TCP 连接，网关应用通过 socket 读取数据。socket 可以理解为程序收发网络数据的接口；应用无需自行实现 TCP 握手或重传。
4. TCP 转发循环与上游协商 SOCKS5 NO AUTH，再发送 `CONNECT`，请求代理连接原始目标。之后在两段连接之间转交数据。
5. 回到手机的 TCP 数据包会按映射恢复为原始远端地址，并修正校验和。因此手机仍认为自己在与网站通信。

```mermaid
flowchart LR
    Client["手机的 TCP 数据包"] --> Hook["IPv4 钩子<br/>记录目标并改写地址"]
    Hook --> Stack["lwIP 本地连接"]
    Stack --> Relay["TCP 转发循环"]
    Relay -->|SOCKS5 CONNECT| Proxy["上游代理"]
    Proxy --> Site["原始网站"]
```

这里“透明”描述的是终端的使用方式，不代表把原始 TCP 数据包直接塞进 SOCKS5。网关先交给 lwIP 处理，再转发连接中的数据；HTTPS 加密仍由浏览器和网站处理，网关不解密网页内容。

## UDP、并发与配置保存怎么处理

UDP 没有 TCP 那样的连接过程，但 SOCKS5 的 UDP 转发仍需要一条 TCP 控制连接。每个 UDP flow 保存客户端和目标信息，协商 `UDP ASSOCIATE`，再将带有 SOCKS5 目标头的 UDP 数据交给中继。回复会核对流和客户端归属后送回对应终端。代码还为 DNS 保留部分容量和协商机会，减少其他 UDP 通信占满资源时无法解析域名的情况。

TCP 和 UDP 分别使用固定的转发循环，检查哪些连接现在能收发，处理一段数据后继续照顾其他连接，不为每条连接创建任务。队列容量和等待时间有上限；较大的 TCP 数据缓冲放在 PSRAM，频繁访问的 TCP 状态保留在内部 RAM。这样控制内存占用，但不会突破无线带宽和活动连接总上限。

配置管理保存当前值与上一份值。新网络配置先试运行，确认后保留；超时恢复旧值。管理和维护任务与转发循环分开，因此查看状态无需为每个网页请求重新启动代理。两台终端各有连接身份和流量计数，用于识别回复归属，并清理已离开终端的连接。

终端必须通过 ESP32 的 DHCP 获取 IPv4 地址：手动设置静态 IP 不会登记到转发客户端列表。连接身份用于防止重连后的旧回复误投，不是防 IP 伪造的认证。流量统计按成功中继的 TCP/UDP **有效载荷字节**累计，不含协议头；不是无线链路总字节数，也不代表应用已收到数据。

管理服务器将每个请求的网络接收/发送期限限制为 **5 秒**，请求体最多 1024 字节。拒绝请求或未读完正文时关闭连接，不会继续排空任意长度的数据，以限制网络阻塞对同一 HTTP 任务中配置回滚的影响。状态刷新会保留尚未保存的 Internet 检查选项，事件名称随页面语言显示。

想阅读源码时，可按下面的顺序寻找对应实现：

| 要了解的部分 | 主要源码 |
| --- | --- |
| 启动、AP/STA 与网络事件 | [app_main.c](../main/app_main.c) |
| TCP 入口、地址改写与目标记录 | [tcp_poc.c](../main/tcp_poc.c)、[packet_rewrite.c](../main/packet_rewrite.c)、[flow_table.c](../main/flow_table.c) |
| TCP 的 SOCKS5 协商和双向转发 | [socks_relay.c](../main/socks_relay.c) |
| UDP 接入、关联和数据格式 | [udp_gateway.c](../main/udp_gateway.c)、[udp_relay.c](../main/udp_relay.c)、[socks_udp.c](../main/socks_udp.c) |
| 终端归属和计数 | [ap_clients.c](../main/ap_clients.c) |
| 配置保存、确认与回滚 | [gateway_settings.c](../main/gateway_settings.c)、[web_management.c](../main/web_management.c) |
| HTTP 传输期限与连接清理 | [management_http.c](../main/management_http.c) |
| 维护任务和连接检查 | [gateway_control.c](../main/gateway_control.c)、[gateway_probe.c](../main/gateway_probe.c) |

更完整的数据路径和模块职责见 [架构说明](architecture.md)。初次使用只需先掌握配置步骤，再按兴趣阅读实现。

详细 CPU / 运行时间统计、RX 缓冲引用跟踪和 Wi-Fi TX 完成观察仅由 `CONFIG_PROXY_VERBOSE_DIAGNOSTICS` 启用；基础 Web 状态 / 错误计数保留。源码默认关闭这些细粒测量，已有 `sdkconfig` 仍保留原值，比较构建时需先检查。没有新的板上测速，不能宣称吞吐提升。

## 运行现有回归测试

测试在开发电脑上编译并执行部分 C 逻辑，因此称为“宿主测试”，无需连接 ESP32 板。先成功构建一次固件，确保锁定的 cJSON 已下载到 `managed_components/`。

在 Linux、已激活 ESP-IDF 的项目终端中，准备系统 GCC/binutils 和 Node.js，再运行：

```bash
# 先记住当前 Python，避免调整 PATH 后切换到另一套解释器。
HOST_PYTHON="$(command -v python)"
PATH=/usr/bin:/bin:$PATH "$HOST_PYTHON" -m unittest discover -s tests -v
node tests/management_feedback_harness.js
node tests/management_transport_harness.js
```

ESP-IDF 的 Python 环境包含 pyserial/esptool，串口相关测试会用到它们。运行完整套件并检查实际执行数量、失败和跳过情况；覆盖固件/工具宿主回归、lwIP 准备脚本及两组前端反馈/兼容测试。准备脚本测试使用独立的临时 Git 仓库，覆盖 LF/CRLF、重复操作、只读检查及拒绝意外修改。HTTP 期限测试使用真实宿主 socket；串口回显/提示符测试使用伪终端和模拟 UART 驱动。请检查实际执行数量、失败和跳过情况；测试通过并不代替真实 Wi-Fi、掉电或长时间稳定性测试。

## 可选的真实浏览器验证

前面的普通 Node 测试不需要浏览器或 npm 依赖。若要在 Linux 上用 Chromium 检查实际 HTML，可把 Playwright 安装到临时的宿主目录。测试通过模拟 API 覆盖中英文桌面/手机布局，不访问开发板。浏览器下载和 Chromium 系统库属于可选开发依赖。

```bash
BROWSER_TEST_TOOLS="$(mktemp -d)"
npm install --prefix "$BROWSER_TEST_TOOLS" --no-package-lock playwright@1.62.1
PLAYWRIGHT_BROWSERS_PATH="$BROWSER_TEST_TOOLS/browsers" "$BROWSER_TEST_TOOLS/node_modules/.bin/playwright" install chromium
NODE_PATH="$BROWSER_TEST_TOOLS/node_modules" PLAYWRIGHT_BROWSERS_PATH="$BROWSER_TEST_TOOLS/browsers" node tests/management_browser_harness.js
```

## tools 工具速览

在项目根目录运行。固件构建只要求 lwIP 准备工具，其余用于可选的排查和开发测试。完整命令、平台限制和测试前提见 [工具使用说明（英文）](../tools/README.md)。

| 工具 | 用途 |
| --- | --- |
| `package_firmware.py`、`flash_firmware.py` | 生成完整/应用镜像及无脚本 ZIP；仓库内可选工具用于校验和分段刷写 |
| `prepare_lwip.py` | `apply` 准备副本，`check` 校验，`restore` 还原本地补丁 |
| `uart_capture.py` | 保存串口日志；Linux/WSL2 可用 `--interactive` 发送命令 |
| `probe_upstream_udp.py`、`probe_upstream_quic.py` | 从电脑直接测试 SOCKS UDP/DNS 或 QUIC，不经过 ESP32 |
| `test_stream_server.py`、`throughput_bench.html` | 在局域网电脑运行测试服务/页面，由 AP 终端测速 |
| `test_transparent_echo.py`、`test-transparent-echo.ps1` | 用专门的 echo 目标测试透明 TCP；普通 HTTPS 网站不是 echo 服务 |
| `collect_link_bench.py`、`snapshot_manifest.py` | 提取支持的诊断串口报告；创建/核对本地文件清单 |

## 项目目录与进一步阅读

| 路径 | 内容 |
| --- | --- |
| `main/` | 固件源码和嵌入的管理页面 |
| `patches/` | lwIP 补丁及版本 / 哈希信息 |
| `tools/` | 补丁准备、串口采集、网络探测和测速工具 |
| `tests/` | C/Python 宿主测试和 JavaScript 前端测试 |
| `experiments/` | 留存的缓冲 / 回包实验模块，供主机测试使用，不链接进固件 |
| `examples/` | Xray 完整客户端示例、字段位置说明及配置片段 |
| `sdkconfig.defaults` | 初次配置的默认硬件和功能参数 |
| `dependencies.lock` | 固件组件版本记录 |
| `docs/` | 架构、维护、性能和开发记录 |

- [文档索引](README.md)
- [维护与恢复](maintenance.md)
- [性能与验证](performance.md)：完整的测量记录、内存采样和验证边界。
- [开发历史](history/README.md)：包含尝试过又撤回的实验，适合研究过程；日常使用优先参考[用户指南](../README.zh-CN.md)及本目录的当前说明。

`sdkconfig`、`build/`、`managed_components/`、生成的 lwIP 副本和测试输出由工具生成。提交代码前检查 `git status`，不要把 Wi-Fi 密码、个人配置、日志或本地固件文件上传到仓库。

## 串口采集

Linux/WSL2 可在项目根目录通过同一连接记录日志，并按行发送命令：

```bash
python tools/uart_capture.py --port /dev/ttyACM0 --no-reset --interactive --timestamps --seconds 0
```

工具的 `--interactive` 标准输入处理仅支持 POSIX，按 Enter 后才发送整行。Windows 输入命令请用 `idf.py -p COM5 monitor` 或串口终端；仅记录日志时可用 `--port COM5`，不加 `--interactive`。带时间戳的采集也会立即显示提示符和回显，无需等待换行。先退出其他串口工具。`--seconds 0` 表示持续读取到中断；`--no-reset` 避免工具主动复位，但 USB 适配器或系统驱动仍可能影响复位信号。
