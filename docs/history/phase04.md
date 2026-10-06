# Phase 4：IPv4 UDP ASSOCIATE

本文件记录Phase4初版。当前有界多association及DNS队列保护实现见 [第七轮](udp07.md)，旧的单control TCP/多源端口结构已经替换。

## 修改文件

- `main/udp_gateway.c/.h`：UDP flow、固定任务、队列、关联控制和 AP 回包。
- `main/socks_udp.c/.h`：IPv4 SOCKS5 UDP 封装/解封装。
- `main/packet_rewrite.c/.h`：只读 IPv4 UDP 校验和/长度解析。
- `main/tcp_poc.c`：复用 input hook，分派 AP UDP 给新模块。
- `main/app_main.c`：STA 状态通知、AP 客户端离线清理、DHCP DNS。
- `main/proxy_config.h`：UDP flow/payload/timeout 与 DNS 常量。
- `main/CMakeLists.txt`、`main/Kconfig.projbuild`、`sdkconfig.defaults`：构建与资源容量。
- `README.md`、本文：当前行为和验收边界。忽略的 sdkconfig 同步配置。

## 结构

TCP 保持 Phase 3 单活动连接的实现。新增一个固定 `proxy_udp` task，6144 字节栈，
非阻塞 select；没有逐 flow task。应用 flow 静态分配 32 条，空闲超时 60 秒。

复用现有、项目内 lwIP input hook：AP 外部单播 IPv4 UDP 在 lwIP 本地目标判断
前被捕获。只读校验原始 IPv4/UDP 校验和和长度，保存完整原始四元组及 payload。
IPv4 caller 负责释放被截获的原始 pbuf。UDP 无 TCP 握手，无需伪装成本地 UDP
listener 再找回逐报文目的地址；不改变 TCP 已验证的改写路径。

单 TCP 控制连接执行 `05 01 00` / `05 03 00 01 00 00 00 00 00 00`。
请求时数据源端口尚未分配，因此关联请求为全零。控制连接关闭/协议错误/STA
离线均关闭 UDP flow，5 秒后在 STA ready 条件下重新建立。关联握手总超时 15 秒。
BND IPv4=0.0.0.0 时使用配置的 SOCKS5 IPv4；非零 IPv4 和非零端口用于 relay。
控制连接保持打开，不为每个 datagram 或 flow 重新建立 TCP association。

**同一关联下，每个 UDP flow 使用独立本地 UDP socket/端口**，并 connect 到
协商得到的 relay 地址。每个收到回包的 socket 唯一对应原始四元组；同时核对
SOCKS5 UDP header 的远端地址与端口。这防止同一 DNS 服务器的多个客户端源端口
互相串包；单个共享 UDP socket 无法仅凭远端四元组正确恢复客户端源端口。

**兼容性未知**：上游必须支持全零 UDP ASSOCIATE 请求之后的多个数据源端口。
如果服务器固定绑定首个 UDP 源端口或不区分来源，当前多流方案可能丢包；此项
尚未在指定上游完成数据面验收。不能把控制连接 ready 当作 32 流兼容性证明。
若服务器要求固定单源端口，需要改变上游/关联数量，或明确降低 UDP 并发语义，
不能静默用“远端地址 -> 最后一个客户端端口”回送数据。
协议依据 [RFC 1928](https://www.rfc-editor.org/rfc/rfc1928)。

回包在 `tcpip_try_callback` 的 TCPIP 上下文注入：组装 UDP 头，使用 lwIP
`inet_chksum_pseudo` 生成 UDP 校验和（计算为零则写 0xffff），由
`ip4_output_if_src` 生成 IPv4 头/校验和并经 AP ARP/netif 发送。
客户端见到的 UDP 源地址/端口仍是原始远端。没有 flow metadata 写入 PCB；
没有重新实现 IPv4 路由、ARP、TCP 或 UDP socket 接收栈。现有 output hook
只处理 TCP，因此这些由 lwIP 生成的 UDP 回包无需二次改写。

输入队列/回包槽各 8 个，缓冲在 PSRAM 中一次分配；队列满、槽满或非阻塞 UDP
send 不可立即完成时丢报文，不引入无界缓存/应用层 UDP 重传。
启动失败释放应用缓冲；运行中的动态 socket、pbuf、callback message 仍由 lwIP
管理，不能据此声称没有任何运行时分配。

## 范围

- IPv4 / NO AUTH / SOCKS5 UDP FRAG=0；ATYP=3/4、RSV 错误、FRAG!=0 丢弃。
- AP IPv4 options、分片不支持；IPv4 UDP checksum=0 合法，非零则验证。
- 最大 UDP payload 1462 字节，为 1500-byte 上游 MTU 留出 SOCKS5 的 10 字节头。
  超限丢弃；不宣称支持任意 UDP/QUIC 大报文或完整 PMTU 语义。
- 不处理广播/组播/P2P 特殊语义；不增加 ICMP 转发或 NAT。
- AP DHCP 下发 DNS 1.1.1.1，需客户端重新获得租约。旧客户端若使用
  192.168.4.1 作为 DNS 仍不支持；固件没有在网关地址运行 DNS server。
- DNS 查询 payload 原样转发，不改 transaction ID、不实现 DNS 解析缓存。
- AP 客户端离线或 STA 状态变化会更新 generation，丢弃过期队列/回包。
- 本阶段初始 TCP 为一个活动流；后续恢复版已改为 32 个，见 [恢复记录](recovery.md)。

## 本机源码与配置

实际读取了本机 v6.1 `ip4_output_if_src` / `inet_chksum_pseudo` /
`tcpip_try_callback` 声明及 `udp.c` checksum=0xffff 处理；DHCP DNS API 对照
官方 `examples/wifi/softap_sta/main/softap_sta.c`。

初始版本 `CONFIG_LWIP_MAX_SOCKETS=40`、`CONFIG_LWIP_MAX_UDP_PCBS=40` 只是容纳 32 UDP
flow 加控制/TCP/系统连接的必要容量，不是吞吐参数调优。
本机 FD_SETSIZE=64，40 sockets 留出 24 个 VFS fd，满足 v6.1 保留至少 6 个
fd 的静态检查，不需要改变 fd_set ABI。Flash / PSRAM / Wi-Fi 参数不变。
现有项目内补丁未修改，`prepare_lwip.py check` 通过，全局 IDF 不变。

## 构建和后续手动验证

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
cd /home/user/esp/esp32-transparent-proxy
idf.py build
idf.py -p /dev/ttyACM0 flash
```

构建成功，无 warning/error；app `0xc15a0`，4 MiB 分区余量 81%。
按用户要求停止性能/交互测试，不再要求运行长下载命令。
编译通过不代表 UDP 数据面、DNS、QUIC 已通过验收。

未来需要功能验收时，Windows 短命令可用：

```cmd
nslookup example.com 1.1.1.1
```

之后再验证同一 DNS 远端的多个源端口、普通 UDP、QUIC 和关联断线重建。
此处只是记录未来验证命令，本轮不要求执行。

## 本次板上启动结果

烧录成功并完成 hash 校验。双核、16 MB Flash、8 MB PSRAM / memory test OK，
AP `192.168.4.1`、STA `192.168.1.103`，AP+STA ready。
没有本次启动时观察到的 crash/watchdog。

实际收到上游失败回复：

```text
udp_proxy: opening persistent UDP ASSOCIATE
udp_proxy: ASSOCIATE rejected VER=5 REP=7 RSV=0 ATYP=1
udp_proxy: association unavailable; close UDP flows, retry after STA ready / 5s
```

`REP=0x07` 为命令不支持：当前 `192.168.1.2:7897` 未接受 UDP ASSOCIATE。
这不是 SOCKS5 UDP 数据已转发的证明；**当前 UDP 未打通**。需要用户提供上游
软件名称/版本，以核对是否 UDP 关闭、监听端口不支持或实现不包含该命令。
不能用 NAT/直连 UDP 偷换 SOCKS5 UDP 功能。TCP CONNECT 路径保持启用。
本轮不追加客户端性能命令或优化参数。

## 已定位 Xray 上游配置

用户提供 Xray `26.1.18`，提交 `7ff06f6`，Windows amd64。
实际只读下载并检查该提交的
[proxy/socks/protocol.go](https://github.com/XTLS/Xray-core/blob/7ff06f6/proxy/socks/protocol.go)：
`cmdUDPAssociate` 分支在 `!s.config.UdpEnabled` 时发送 `statusCmdNotSupport=0x07`。
这与固件记录的有效 CMD=3 请求和 REP=7 回复吻合，指向该监听入站 UDP 未启用。
不是据最新版源码猜测该版本；Web 抓取历史 raw 文件失败后，通过直接只读下载
成功核对了用户给出的提交。

官方 [SOCKS 入站文档](https://xtls.github.io/config/inbounds/socks.html)
说明 `udp` 默认 false；`ip` 是 UDP 客户端可用于抵达服务器的地址。
监听 7897 的既有 SOCKS 入站 `settings` 应合并以下字段：

```json
{
  "auth": "noauth",
  "udp": true,
  "ip": "192.168.1.2"
}
```

片段另存 `examples/xray-socks-settings.json`。它不是完整 Xray 配置；不要将其
替换为整个配置文件，不修改既有出站、路由或上游传输配置。
如果 GUI 生成配置，应通过其对应设置/持久配置启用，避免手工修改临时文件后
被覆盖。重新加载上游配置后，ESP32 会自动重试关联，不需要重新烧录或跑性能命令。
用户上游运行环境不在当前可编辑的 WSL 工作区内，因此尚未替用户应用该修改。

### 用户修改配置后的重试

用户要求重试后，仅进行了关联握手检查，未跑性能测试：

```text
TCP7897 connected
NO AUTH: 0500
UDP ASSOCIATE: 050000017f0000011ed9
```

REP 已由 0x07 变为 0x00：上游现在接受 UDP ASSOCIATE。
但 BND.ADDR/BND.PORT 为 `127.0.0.1:7897`，仍不是 ESP32 可达的 relay 地址。
固件的回环地址校验会关闭该关联；这解释了串口不再打印 REP=7、但仍未 ready。
不要把 `127.0.0.1` 发给 AP 客户端或连接 ESP 自身的回环地址。
需要核对实际运行的 7897 入站 `settings.ip` / GUI 生成配置，确保返回
`192.168.1.2`。尚未确认 UDP 数据端口可达或完成 DNS 转发。

### 再次修改 IP 后的重试

ESP32 自动重试已成功，无需重新编译或烧录：

```text
I (655568) udp_proxy: opening persistent UDP ASSOCIATE
I (655578) udp_proxy: UDP ASSOCIATE ready relay=192.168.1.2:7897 flows=32
```

UDP 控制关联和可达地址协商已完成，旧 REP=7 和 loopback 地址问题已解除。
这仍不等于 DNS/普通 UDP/QUIC 数据面已通过验收；本次未要求客户端测试，
也没有进行性能优化。

后续恢复版 sockets=104 / active TCP PCBs=80 / FD_SETSIZE=128；DNS 和 NTP 已收到客户端成功反馈，
QUIC 未验证。最新配置、固件及恢复验证状态以 [恢复记录](recovery.md) 为准。
