# 全项目复审：DNS 路径、Xray 升级兼容性与优化顺序

日期：2026-10-02。此次是源码、配置、历史板上日志与官方资料审查，未修改或烧录固件，未替用户修改正在运行的 Xray。

## 结论

还有改进空间，但应先修正确性与恢复路径，再测带宽。当前不能把 APP 首页延迟归结为 ESP32 硬件带宽上限或 TCP 并发数不足。

最重要的两个发现：

1. 用户提供的 Xray 配置虽然定义了国内/境外 DNS，但没有把 ESP32 转发的明文 DNS 请求路由到 DNS outbound。这个配置片段下，客户端 DNS 不会自动使用所列的阿里/Cloudflare DoH 分流与缓存。
2. 最新 Xray 的 UDP ASSOCIATE 已锁定一个客户端 UDP 源端点。当前固件用一个 association、每 flow 一个 UDP socket，在该实现下不兼容。旧版 NO AUTH 模式可接受多个源端口，不能据此保证升级后仍可用。

第二项是升级兼容性结论，不是对当前运行版本的远程识别；尚不能把历史故障直接归因于新版 Xray。

后续用户已确认当前是v26.1.18，并纠正为proxy之后已有direct/freedom，因此撤回缺失direct的诊断。第七轮已实现有界多association与DNS调度，原实现的缺陷和待办以当时审查状态阅读；最新验证和上游配置见 [第七轮](udp07.md)、[Xray合并说明](upstream-xray07.md)。

## 审查范围与版本

重新阅读 main 下所有 C/header、Kconfig/CMake/defaults、项目 IPv4 patch 及应用脚本、host 测试与工具。核对本机 IDF v6.1 的 ip4.c、TCP/lwIP Kconfig、软件 checksum 和任务配置。历史测量取自第四/第五轮文档与对应本地日志，不伪装成本轮新测量。

2026-10-02 查询官方 GitHub API 与源码：

| 版本 | 核对结果 |
| --- | --- |
| v26.1.18 | 用户此前报告的版本；直接读取该 tag 的 SOCKS、DNS outbound、DNS 配置/DoH 源码 |
| v26.3.27 | GitHub `releases/latest` 当前返回的非预发布版；DNS outbound 配置结构仍与 v26.1.18 一致 |
| v26.9.30 | 最新发布，2026-09-30 发布，GitHub 标记 pre-release |
| main | 此次读取时为 `b26a91de4f3294e26a0ad0a970b81a386a41f789`，与 v26.9.30 发布提交相同 |

“最新正式版”与“最新发布/主分支”在这里不同。结论锁定上述提交，不保证未来版本不再修改行为。
来源：[最新发布](https://github.com/XTLS/Xray-core/releases/tag/v26.9.30)、[非预发布 latest API](https://api.github.com/repos/XTLS/Xray-core/releases/latest)。

## 高优先级：UDP ASSOCIATE 的跨版本设计限制

本地 `main/udp_gateway.c:203` 为每个完整 UDP 四元组创建独立 socket；worker 只有一个 control TCP，ASSOCIATE 请求为 `0.0.0.0:0`。因此它会从多个 STA UDP 源端口向同一个 relay 发送。

v26.1.18 的 SOCKS UDP 监听共享入站端口；NO AUTH 时没有启用基于认证的 UDPFilter。此前每 flow socket 的做法利用了该实现的宽松行为。

最新代码的行为：

- `proxy/socks/protocol.go` 为每次 association 建立独立临时 UDP listener，回复实际的 BND.PORT。固件已读取回复的端口，这一点是正确的。
- `proxy/socks/temp_udp_listen.go` 初始允许端口为 0；接收第一个来自预期 IP 的 UDP 包后，保存该包的 IP:port。后续只接受完全相同的源端点。
- `proxy/socks/server.go` 给该 UDP listener 设置 connection-idle timer；UDP 空闲会关闭 UDP 和关联的 TCP。仅有 TCP keepalive 不会刷新这个 UDP 活动计时器。

因此，当前固件在最新版上可能只有第一个 UDP flow 工作；即使它是 DNS，5 秒回收后新建 socket 也可能换源端口，导致后续 DNS 无响应。新监听器还使用动态 UDP 端口，上游防火墙若只允许 UDP/7897，同样可能阻断数据。不能把所有 UDP 无回包一概判断为公网服务器不支持 UDP。

来源：[新版握手](https://github.com/XTLS/Xray-core/blob/b26a91de4f3294e26a0ad0a970b81a386a41f789/proxy/socks/protocol.go)、[端点锁定与 timer](https://github.com/XTLS/Xray-core/blob/b26a91de4f3294e26a0ad0a970b81a386a41f789/proxy/socks/temp_udp_listen.go)、[association 生命周期](https://github.com/XTLS/Xray-core/blob/b26a91de4f3294e26a0ad0a970b81a386a41f789/proxy/socks/server.go)、[旧版 server](https://github.com/XTLS/Xray-core/blob/v26.1.18/proxy/socks/server.go)。

### 为什么不能直接合并成一个 UDP socket

例如两个应用同时发往同一个目标：

```
192.168.4.2:51001 -> 1.1.1.1:53
192.168.4.2:51002 -> 1.1.1.1:53
```

若共享一个 UDP socket/association，两类回复中的 SOCKS5 地址头都只有 `1.1.1.1:53`，没有原客户端端口。不能通用地判断回包应交给 51001 还是 51002。DNS 可另做 ID 映射，但这不能解决任意 UDP/QUIC；即使只接一个 AP 客户端也存在此问题。[SOCKS5 UDP 格式与 association 定义](https://www.rfc-editor.org/rfc/rfc1928.html)。

建议下一轮选择有界的每 UDP flow 一个 association（仍是单 event loop，无每 flow task），或按原客户端 UDP 源端点复用 association，并明确同一目的重用的生命周期。最容易验证的是每 flow 一个 association；这会调整原 V1“只用一个 control TCP”的约束，不能伪装成一个小参数优化。

若保留 64 TCP relay 和 32 UDP flow，最多为 128 个 TCP relay socket + 32 UDP socket + 32 control TCP + 1 listener = 193 个 socket，尚未计其他使用者。当前 `LWIP_MAX_SOCKETS=176` 不够，需要重新预算（例如 208 起步，检查 FD_SETSIZE/VFS 范围、PCB、heap），或降低活动容量。不能只新增 control socket 而维持原容量保证。

严格维持单 association，只能限制可同时存在的 UDP 源/目的组合或增加协议专用复用；不满足任意 UDP 的透明语义。此次未做这种架构变更。

## DNS 实际路径及用户配置

当前 DHCP 下发 DNS `1.1.1.1`，ESP32 不是 DNS resolver。`dns_flow.c` 只跟踪报文 ID，用于回收已完成查询，不提供解析、缓存、域名规则。

```
客户端明文 DNS -> ESP32 透明 UDP/TCP -> Xray SOCKS 入站
                                        |
                         当前：普通目标 1.1.1.1:53 的路由
                         建议：DNS 专用路由 -> DNS outbound
                                               -> 内置 dns.servers
```

需要分开理解三种能力：SOCKS5 CONNECT 可携带域名、SOCKS5 可运送 DNS 报文、Xray 内置 DNS 可按域名分流。三者不会自动互相启用。项目目前 CONNECT 使用 ATYP=IPv4，不能从一个任意目标 IP 可靠恢复原域名；`IPIfNonMatch` 也是域名解析为 IP 后匹配，不是反向恢复域名。

用户给出的入站没有 sniffing，路由也没有 DNS outbound。因此依据已提供片段，`geosite:cn` 不会直接匹配大部分 ESP32 发来的 IP 目标；`geoip:cn` 仍可匹配。未提供完整 outbounds，不能判断未匹配流量具体走哪个 tag；默认取第一条出站，并不必然等于代理。

### 已准备可合并的配置片段

- `examples/xray-esp32-dns-route.json`：放在 `routing.rules` 最前面，优先于 private/cn IP 规则。按 Xray 看到的 ESP32 STA 源 IP `192.168.1.103/32` 限定作用范围；不是 AP 客户端的 192.168.4.x。应固定这个 DHCP 租约或在地址变化后同步调整规则。
- `examples/xray-esp32-dns-outbound-compat.json`：加入现有 `outbounds` 的末尾；适用于已核对的 v26.1.18/v26.3.27/v26.9.30。新版本会提示 `nonIPQuery` 已弃用，但此次源码仍支持。
- `examples/xray-esp32-dns-outbound-current.json`：已核对的最新版本写法，显式用 `rules` 处理 A/AAAA 与其他类型。旧版本不支持该规则结构，不能拿去依赖旧版行为。

两种 outbound 只选一种，tag 均为 `esp32-dns`。这些是合并片段，不能用它们覆盖完整 Xray 配置，也不要把 DNS outbound 放到第一条成为默认出站。合并前检查 tag 未重复。

二者意图一致：A/AAAA 交内置 DNS；其他查询类型放行给原目标 DNS。最新版 `direct` 是 DNS outbound 的动作名，不是用户路由的 `outboundTag: direct`。放行的查询不会享受内置 A/AAAA 分流，其实际连通性也需要验证；不能保证所有 TXT/PTR/HTTPS/SVCB 查询都按域名分流。若希望改为立即返回空答案，最新版原生支持，但这是另一种 DNS 策略，不在此次默认片段里擅自添加。

最新源码还改变了空 `settings` 的默认行为：A/AAAA 导入内置 DNS，其他类型返回 RCODE=0 空答案；所以不能声称在所有版本上 `settings:{}` 都等价于旧版的放行策略。新 `rules` 不可与旧 `nonIPQuery`/`blockTypes` 混用。

来源：[最新 DNS outbound 配置解析](https://github.com/XTLS/Xray-core/blob/b26a91de4f3294e26a0ad0a970b81a386a41f789/infra/conf/dns_proxy.go)、[默认规则与处理](https://github.com/XTLS/Xray-core/blob/b26a91de4f3294e26a0ad0a970b81a386a41f789/proxy/dns/dns.go)、[旧版配置解析](https://github.com/XTLS/Xray-core/blob/v26.1.18/infra/conf/dns_proxy.go)、[官方 DNS outbound 文档](https://xtls.github.io/config/outbounds/dns.html)。

现有 `dns.servers` 可以先保留，先验证请求确实进入内置 DNS。后续须核对：

- 阿里的 `https+local://` 使用本机直接连接；Cloudflare 的 `https://` 经过 Xray routing，不自动保证走代理。DoH 域名的首次解析、实际默认 outbound、其可达性必须与完整出站配置一起看。官方特别提示域名形式的非 local DNS 可能解析回环；可使用 DNS hosts 引导，或 Cloudflare 支持的 `https://1.1.1.1/dns-query` 避免先解析 DoH 域名，再明确该请求的出站。此次未拿不完整的 outbounds 生成一个假定可直接覆盖的整份配置。
- `disableFallback:false` 加 `localhost` 并非严格的国内/国外隔离。匹配服务器失败后可尝试其他服务器。`disableFallbackIfMatch:true` 在核对的旧版和新版均存在，可约束匹配域名的回退，但也会降低失败时的可用性，不能不说明取舍就开启。
- 给 SOCKS 入站增加 `sniffing` 可从部分 HTTP/TLS/QUIC 流量提取域名供路由。`routeOnly:true` 只影响路由、不改实际目标 IP，不能修复已经解析出的不适合地址；关闭 routeOnly 又会改变目标/解析行为。该入站被多终端共用，不宜为了 ESP32 一次性改变所有客户端。
- Android 私人 DNS、浏览器 DoH/DoT 不匹配明文端口 53 规则。尚未确认该 Android 的设置，不能把所有 APP 延迟都解释为 DHCP DNS。

来源：[DNS 配置](https://xtls.github.io/config/dns.html)、[新源码的 fallback 选择](https://github.com/XTLS/Xray-core/blob/b26a91de4f3294e26a0ad0a970b81a386a41f789/app/dns/dns.go)、[旧版 DoH 实现](https://github.com/XTLS/Xray-core/blob/v26.1.18/app/dns/nameserver_doh.go)、[sniffing](https://xtls.github.io/config/inbound.html)、[routing](https://xtls.github.io/config/routing.html)。

DNS 分流/缓存放在上游 Xray 即可，不要求 ESP32 加入本地 DNS server 或 GeoSite。它可能减少解析延迟、改善 CDN 地址与出站地域的匹配，但修不好 ESP32 到 Xray 的控制连接中断。

## 固件其余发现，按处理优先级排序

| 优先级 | 位置与证据 | 影响与建议 |
| --- | --- | --- |
| 高 | `udp_gateway.c:339`：控制连接失败后固定等 5 秒，再尝试最长 15 秒握手；失败清空所有 UDP flows/队列 | 已有日志显示 association 中止和多轮握手超时，这是已观测的 DNS 中断来源。区分正常 idle EOF、链路故障与协议拒绝；首次恢复可尽快尝试，连续失败有界退避，等待数据须设最大年龄。不能盲目关闭 keepalive 掩盖死连接 |
| 高 | `udp_gateway.c:128,155`：4 条 DNS flow 保留不等于入队/回包资源保留；所有 UDP 共用输入 16、delivery 8 | UDP 突发仍可能在进入 flow 表前或回包时挤掉 DNS。后续做有界 DNS 优先调度/预留队列，保证普通 UDP 仍获得服务；补实际 worker 恢复与拥塞回归 |
| 中 | `socks_relay.c:18,111,162,308`：accepted/CONNECT ready/closed 全部 INFO，由单个 relay worker 输出 | 短连接密集时 UART/日志锁会占用 worker。建议成功连接细节降 DEBUG，错误保留，定时聚合。115200 的串口预算约 11.5 KB/s；具体任务阻塞占比尚未 profile |
| 中 | `socks_relay.c:162`：只记录 SOCKS 回复耗时 | 新旧 Xray 都先写 SOCKS success 再调用出站 dispatcher，不能把 handshake_ms 当目标 TCP 连通或网站 TTFB。补从首个请求字节到首个上游字节的分阶段计时、DNS 按 ID RTT；避免逐包日志 |
| 中 | `socks_relay.c:204`：120 秒 idle 走 abort/RST | 应用复用闲置连接可能撞上 reset；不是目前所有 reset 的证据。可比较压力驱动清理与合理 idle 策略，必须保留背压/半关闭测试，不能只无限延长占满 64 路 |
| 中 | `flow_table.c:58`：未被 accept 的 mapping 空闲 2*MSL 可过期，而 socket_open 仅在 accept 后设置 | 条件性风险：64 路持续忙，backlog 内已 ESTABLISHED 的连接静默等待超过 120 秒，PCB 可能还在而映射先过期。需增加超载等待生命周期测试，不把 SYN_RCVD 的超时当作所有待 accept PCB 都有相同超时 |
| 中 | `sdkconfig` 的 OOSEQ_MAX_PBUFS=4，defaults 没固定该项；v6.1 在当前 PSRAM 条件下默认 0 | 新建配置与现在固件不同，且 0 表示不限条数。先固定可重现基线，再结合重传/乱序/内存评估 SACK/OOSEQ；不能因 PSRAM 空闲就盲目设无限 |
| 中 | UDP payload 上限 1462，IP 分片/options 与 SOCKS FRAG 非零丢弃 | 大报文可能无回复，当前不生成相应 ICMP/调整 DNS TC。限制必须保留在兼容性说明；小 DNS/NTP 成功并非任意 DNS/QUIC 验收。现有窗口 big=0，未证实它造成此次 APP 故障 |
| 后续 profile | `tcp_poc.c` 私有 pbuf clone，input checksum；UDP 多次固定长度复制，PSRAM 中的 flow 表在临界区扫描 | 可比较固定 buffer pool、按实际长度复制、分批过期扫描。不能直接移除 output clone，会损坏 lwIP 重传或 ARP 持有的 pbuf；input 移除 checksum 校验会让错误报文参与 flow 创建 |

还存在维护项：`PROXY_MAX_TCP_FLOWS` 与 `SOCKS_MAX_SESSIONS` 各自写 64；AP 地址在数处硬编码；AP/STA 子网重叠只警告。它们当前配置下不是已证实瓶颈，应在后续变更容量/地址时一并统一。

### 已确认合理的部分

lwIP 继续负责 TCP 状态机；flow metadata 独立于 PCB，完整四元组+内部 peer port 避免同源端口冲突；双方向有界队列提供背压，EOF/部分写正确处理；没有每连接 task。
UDP 的 TCPIP 回调拥有独立数据副本，eventfd 唤醒避免 TCPIP 内调用 netconn 导致自锁，代际隔离避免大多数重连后的旧回包串到新客户端。仍应补 select 唤醒后代际变化与 association 重建的并发回归。

本机 hook 确认在 ip4.c 的本地接收判定之前，因此可以接管原本不是发给 ESP32 的目标；它也在重组之前，目前依靠显式拒绝分片。不是原需求中理想的“重组后 hook”；如果将来支持分片，还必须处理目标地址接收判定，不能只挪一个函数调用。

## 带宽与资源判断

CPU 已为双核 240 MHz/O2，Wi-Fi PS_NONE、AMPDU 开；AP+STA 共用射频/信道。当前 RX static16/dynamic48、TX static16/cache32、BA16；TCP window23040、snd17280、recv mailbox18、TCPIP mailbox48。64 relay 的应用缓冲约 1 MiB PSRAM，lwIP 的 socket/窗口占用另计。

历史用户报告单次 8 MiB 下载约 500K/15秒；上一轮板上采样 internal 空闲仍约 210 KiB、PSRAM 约 6.7 MiB。不是同时跑满 64 连接的内存峰值，也没有 CPU 利用率测量，不能据此断言硬件已满载或还能提高几倍。500 KiB/s 约 4.1 Mbit/s，不能与官方单接口实验室 iperf 峰值直接比较。

官方建议同时权衡 Wi-Fi 缓冲、TCP 窗口和内存，而不是单项最大化。当前优先处理上述 UDP 正确性和延迟；之后在同一信道、同一上游下比较 AP+STA LAN 基线、透明 echo、TCP SOCKS、UDP。IRAM 优化在 v6.1 Kconfig 标明普通项约 10 KiB 且双核收益有限，TCP extra 项约 17 KiB；无 A/B 结果前不启用。[Espressif 性能与缓冲说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)。

## 本轮验证与文件

- `prepare_lwip.py check` 通过：项目 override/patch 完整，全局 ESP-IDF 未修改。
- IPv4/TCP rewrite 7 项、实际 C TCP relay 11 项 host 测试通过；flow_table 与 DNS ID harness 均通过。
- EIM 环境下 `idf.py build` 成功；日志 `build-review06.log` 无编译 warning/error；app 785216 bytes (`0xbfb40`)。
- ELF SHA256 `d887232f44b23aade2ec186948ed16caadf3d32b39a6a5744699bdf9c0b8cf47`，与上一轮最终固件一致。
- 配置片段完成 JSON 语法检查和源码字段核对，尚未在用户 Windows Xray 上加载；没有声称做过该版本实际 UDP 兼容运行测试。
- 当前自动化主要覆盖 TCP 和 DNS flow metadata，缺少 production UDP worker、UDP rewrite/codec、association source-port 约束、故障恢复的完整测试。此次不把编译和 TCP 测试算作这些路径已验证。

复现命令：

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check
python tests/test_packet_rewrite.py -v
python tests/test_socks_relay.py -v
gcc -std=c11 -Wall -Wextra -Werror -I main tests/flow_table_harness.c main/flow_table.c -o tests/.out/flow_table
tests/.out/flow_table
gcc -std=c11 -Wall -Wextra -Werror -I main tests/dns_flow_harness.c main/dns_flow.c -o tests/.out/dns_flow
tests/.out/dns_flow
idf.py build
```

仅新增本审查文档、3 个 Xray 配置片段，更新 README 索引。暂不要求重复客户端测速。后续应先确定 association 改造范围、接通上游 DNS 路由；再由 Android 自然操作复测配合分阶段指标，最后比较吞吐。
