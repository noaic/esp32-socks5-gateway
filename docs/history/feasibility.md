# 本机 v6.1 可行性分析

结论：小规模 IPv4 TCP/UDP 透明 SOCKS5 网关在设计上可行，但尚未验证透明数据面。
ESP32-S3 AP+STA 不是双射频路由器，不能承诺吞吐或完整软路由行为。
保留 lwIP TCP 状态机并在 IPv4 层双向改写符合目标；不需要 Linux TPROXY。

## 已检查的源码

ESP-IDF tag `v6.1`；esp-lwIP commit
`c6f2f878e7b0f86033214b85547d579be43351e3`。
路径均相对于 `/home/user/.espressif/v6.1/esp-idf`：

| 文件 | 本次检查结果 |
| --- | --- |
| `components/lwip/lwip/src/core/ipv4/ip4.c:487` | `ip4_input()` 起点 |
| 同文件 `:521` | `LWIP_HOOK_IP4_INPUT` 在 header 长度和 IPv4 checksum 检查前，过早 |
| 同文件 `:577` 附近 | 复制目的/源地址至 `ip_data`，随后做本地接收接口判定 |
| 同文件 `:691` 附近 | `netif == NULL` 时先 forward/drop；到不了后面的 reassembly/dispatch |
| 同文件 `:715` | `ip4_reass()` 只对已被判定本地接收的包执行 |
| 同文件 `:780`, `:786` | 去掉 IPv4 header 后调用 `udp_input()` / `tcp_input()` |
| 同文件 `:917` | `ip4_output_if_opt_src()`，构造 IPv4 header 和 checksum |
| 同文件 `:1083` 附近 | 超 MTU 时先 `ip4_frag()`，否则 `:1091` 送 `netif->output()` |
| `components/esp_psram/esp32s3/Kconfig.spiram` | Octal/80 MHz 等真实 Kconfig |
| `components/esp_psram/Kconfig.spiram.common` | 初始化、memory test、malloc 策略 |
| `examples/wifi/softap_sta/main/softap_sta.c` | 官方 APSTA/netif/DHCP 事件 API |

本机 lwIP 未发现 `LWIP_HOOK_IP4_OUTPUT`。Phase 2 不能假定该 hook 已存在。

## 必须修正的输入设计假设

“校验/重组后、dispatch 前添加一个 hook”单独不能实现任意远端 IP 的透明接收：
外部目标包已经在前面的本地接收判定处被丢弃。
Phase 2 暂不支持分片时，优先在 IPv4 长度/checksum 校验后、
目的地址复制和本地判定前选择性 rewrite；只接受 AP 入站、有效 unicast TCP，
先丢弃带 MF/offset 的被选中报文。这样改写后的地址会自然进入 `ip_data`。
若后续要在重组后改写，还需要提前允许所选包本地接收，重组后更新
header 指针/长度、`ip_data.current_iphdr_dest` 等缓存，重新验证接收上下文。
DHCP、发往设备本身的管理流量、广播/组播和 STA 上游流量必须绕开代理。

## 输出、校验和与 pbuf

输出改写需在 IPv4 header 和运输层 checksum 都就绪后、fragment/输出前进行，
限定 AP 出站且命中反向 flow，避免影响 SOCKS5 socket 的 STA 出站。
在分片之后改运输层 checksum 会遗漏没有 TCP/UDP header 的分片。
PSRAM/pbuf chain、引用计数和共享 payload 不能按连续可写内存假定。
尤其 TCP 重传复用 pbuf：输出 hook 的改写不能污染 lwIP 保存的原始 TCP segment；
应考虑 copy-on-write 或可证明安全的改写/恢复生命周期，包括 ARP 排队引用。
IPv4 checksum 和 TCP/UDP pseudo-header checksum 都需正确处理；UDP 原始 checksum=0
代表 IPv4 未启用校验，非零 checksum 更新后的数学零值需编码为 0xffff。
应先验证原报文 checksum，不能重算而掩盖收到的损坏数据。

## flow table 不能只靠 client IP:port

不同原始远端连接可能复用同一个 client IP:port。若全部映射到同一个
AP IP:listener port，原来不同的 TCP 4-tuple 会坍缩成同一个 lwIP 4-tuple。
Phase 2 单 flow 可先限制；Phase 3/32 flows 需要分配可区分的本地端口等映射策略，
并在 accept 时用 peer 和 local endpoint 查表。不得把 metadata 放入 PCB。
FIN 后不能立即复用 mapping，须考虑 retransmission、half-close 和 TIME_WAIT。
hook 属于 TCPIP 上下文，socket event loop 属于另一上下文；表访问和生命周期必须同步，
hook 不得阻塞或执行 SOCKS5 I/O。

静态 32 TCP / 32 UDP 元数据表可行；socket/TCP buffer 仍由 lwIP 动态分配。
TCP 32 flows 最少需要 64 个连接 socket 再加 listeners/control，因此必须在
Phase 3 核对 `LWIP_MAX_SOCKETS`、TCP PCB 数量、poll/select 限制及 internal RAM。
不能以 8 MB PSRAM 代替 internal/DMA 内存预算；吞吐和 32 活跃 flows 待压测确认。

## SOCKS5 与 UDP 边界

Phase 3 按非阻塞状态机处理 CONNECT、短读短写、握手失败、backpressure、
EOF/half-close/RST/timeout，一个固定 event loop 即可，无需一连接一 task。
SOCKS5 CONNECT 成功前须有界缓冲客户端数据。

Phase 4 必须确认上游实际支持 UDP ASSOCIATE 且 relay 对 STA 可达；
NO AUTH/CONNECT 可用并不证明 UDP 可用。保留 control TCP connection，
处理断开后关联重建、BND 地址/端口、来源过滤和 SOCKS5 header 开销导致的 MTU 问题。
请求 destination IPv4 only 与服务端回复 BND ATYP 是不同的限制，须明确对
非 IPv4 BND 回复的失败策略。FRAG 非零丢弃。有些 UDP 应用使用 IPv4 fragmentation，
V1 分片策略需在 Phase 4 明确。QUIC 可承载不等于保证其吞吐表现。
DNS 首测可先由客户端显式查询一个外部 IPv4 DNS；若最终 DHCP 把 DNS 指向
192.168.4.1，则还需 DNS 本地处理策略，不能假定透明 UDP 自然覆盖它。

IPv4 only 对可回退 IPv4 的终端/应用有效；不能让只支持 IPv6 的服务可达。
当前范围也没有 ICMP 代理，因此外网 ping 不能用作 SOCKS5 验收依据。

## 后续 patch 约束

Phase 0/1 无需 patch。Phase 2 再确定最小 diff 并保存到项目 `patches/`。
优先将 IDF lwIP component 复制到项目构建覆盖目录（不使用修改源码的 symlink），
在项目副本应用 patch；脚本核对 IDF/lwIP revision，正向 `git apply --check`，
反向检查判断已应用，检测冲突并停止。README 必须给出 reverse-check/reverse-apply
恢复步骤和重新应用命令。此处仅记录约束，没有提前实现或修改全局 SDK。

后续已完成 TCP/UDP基础数据面验证。共享 listener 的 client IP/port 冲突
在 [第一轮优化](performance01.md) 中使用独立内部 peer port 和 full 4-tuple 映射修正；
此文件保留 Phase0/1 初始可行性分析，最新验证状态以对应阶段记录为准。
