# 第七轮：有界 UDP association 与 DNS 调度

后续已完成DNS保留时间改为30秒，并增加分段延迟统计与空闲TCP正常关闭，当前结果见[第八轮](latency08.md)。本文的5秒为当时基线。

## 修改与架构

按用户“请继续”的指示落实第六轮发现。把一个全局 UDP association 改成每个完整 UDP 四元组一个 control TCP + 一个固定源端口 UDP socket，最多32路，仍由单 `proxy_udp` task/select 驱动，没有每 flow task。原 V1 单 control TCP 的约束已调整，以正确支持普通 SOCKS5 服务器及新版 Xray 的源端点绑定。

`udp_relay.c/.h` 是可在 host 上直接运行的实际 production socket 状态机，`udp_gateway.c` 保留 ESP-IDF 的入包、FreeRTOS队列、eventfd、TCPIP回包适配。仅抽出需要验证的协议/恢复核心；lwIP patch、TCP 状态机、独立TCP mapping架构保持。

- 每个 UDP socket 在 ASSOCIATE 前 bind，请求填写 control TCP 的本机 IPv4 与已绑定的 UDP port；数据 socket 保持该源端口。接收协商的动态 BND.PORT；BND.IP=0 时使用上游IP。
- 每路 association 仅影响自己的 flow。控制 EOF/错误关闭该路两种 socket，未发送数据保留在有界队列；第一次恢复尽快尝试，连续失败使用250～5000ms有界退避，无需等待统一5秒。已发送UDP不自行重传。
- 每 flow 固定4包待发，全部在 PSRAM 中一次分配；握手期间保留首包，UDP send EAGAIN 保留等待下次可写。超过3秒的未发报文丢弃并计数，不在恢复后发出陈旧包。
- 同时握手最多8路，普通UDP最多占其中6路，DNS优先并有2路握手保留。避免大量无响应普通流使DNS连 association 都无法建立。
- 普通输入队列16、独立DNS输入队列8；每轮优先处理4个DNS再4个普通包，空余预算可借给另一类。回包8个固定槽中2个保留给DNS；普通包不能占满全部槽。
- 每个可读UDP socket最多收4包，先DNS再普通、轮转flow索引，减少逐包select开销并限制单路工作量。
- 保留32 flow中的4个DNS admission槽和已完成DNS的5秒空闲/满表回收。没有对正在等待匹配ID的DNS简单按端口53强制回收。
- select 返回后立即重读STA/client generation，旧代际socket不继续发送/处理回包；TCPIP提交时及实际回包时再校验代际。
- DNS报文原样转发，跟踪8个ID的首发时间，输出匹配响应的RTT汇总；重试相同ID不重置首发时间。RTT不包含建立association前的待发时间，后者由queue_wait单独统计；提交回包不等于客户端已经收到。

TCP成功accepted、SOCKS reply、closed细节降DEBUG，错误和定时统计保留。新增首个请求成功写出至首个上游数据收到的耗时汇总；这个TCP指标可能是TLS ServerHello或应用响应，不是完整页面加载时间/HTTP正文TTFB。无请求的server-first banner不计该指标。
STA未ready时不继续accept新连接然后立即RST，保留在lwIP有限backlog等ready；现有连接仍在network generation改变时终止。backlog超载/长等候metadata风险仍需后续专门处理。

## 配置和资源

`LWIP_MAX_SOCKETS` 从176变为208，`FD_SETSIZE=256` 保持全组件一致；v6.1实际 socket offset为48，VFS/eventfd仍有空间。最坏活动应用socket预算：64*2 TCP relay + 32 UDP + 32 control TCP + 1 listener =193，208留15个余量。
`MAX_ACTIVE_TCP=176` 与 `MAX_UDP_PCBS=40` 保持，新增静态检查覆盖各项容量；关闭/TIME_WAIT与实际lwIP分配仍需压力验证，预算不是峰值heap保证。

`LWIP_TCP_OOSEQ_MAX_PBUFS=4` 固化到defaults，保持当前运行基线，避免新建配置在PSRAM条件下意外变成0（无限）。其余Wi-Fi buffer/window/PSRAM/CPU/watchdog参数延续上一版；删除未使用的重复TCP容量宏。

新增32*4包pending约185KiB，加其他flow metadata；启动后PSRAM实测约6.63MiB空闲。固定应用buffer无每flow malloc，socket/pbuf等仍由lwIP动态管理，不声称完全零运行时分配。

## 上游配置新发现

用户确认正在使用Xray26.1.18，第一条为tag=proxy、后面已有tag=direct/freedom；最初“唯一proxy”的回答已被用户纠正。缺失direct的诊断撤回，不作为当前APP故障原因。现有路由缺少把客户端明文DNS接入内置DNS模块的规则，建议只补这部分。

完整合并说明与最短验证见 [上游配置](upstream-xray07.md)。新增DNS设置/DoH路由片段，保留现有proxy/direct。明文DNS路由只匹配ESP32 STA源IP，DoH带独立DNStag并明确选择proxy，Cloudflare使用IP形式endpoint避免先解析DoH域名；阿里继续local模式。未代用户远程修改Windows配置。

本轮不添加本地DNS服务器、GeoSite到ESP、sniffing、UDP443封禁、BLE/WebUI、IPv6或分片。当前上游是旧版，因此新association改造是兼容性修复，不用它解释所有旧故障。

## 文件列表

| 文件 | 用途 |
| --- | --- |
| main/udp_relay.c/.h | 有界的多association、非阻塞socket状态机、待发与恢复、DNS优先/RTT |
| main/udp_gateway.c | ESP队列/eventfd/TCPIP回包适配，DNS队列与delivery预留 |
| main/dns_flow.c/.h | 保留现有ID跟踪，增加首发时间/匹配RTT |
| main/socks_relay.c | 成功日志降级、首响应指标、STA未ready时等待accept |
| main/proxy_config.h | 删除未用重复TCP容量宏 |
| main/CMakeLists.txt | 注册实际UDP relay核心 |
| sdkconfig.defaults | socket208及显式OOSEQ4；本机忽略的sdkconfig同步 |
| tests/udp_relay_harness.c、test_udp_relay.py、udp_send_shim.c | 执行production核心的socket集成回归；shim仅测试一次EAGAIN |
| tests/test_udp_packets.py | production UDP parser/codec边界、checksum及FRAG等拒绝 |
| tests/dns_flow_harness.c、test_socks_relay.py | RTT、重试/乱序与网络ready等待回归 |
| examples/xray-dns-settings.json、xray-doh-route.json | 当前上游可合并片段 |
| README.md、docs/udp07.md、docs/upstream-xray07.md、docs/code-review06.md | 当前实现、验证、配置及审查结论更新 |

## 验证

- 11项实际UDP核心集成测试通过：严格源端点绑定、相同DNS目标/相同ID多个客户端源端口隔离、同flow源端口复用、动态relay端口、旧共享relay、BND.IP零回退、分段reply、单路control EOF恢复、REP拒绝后重试、一次send EAGAIN无丢失/重复、待发上限与年龄、DNS容量及完成查询回收、普通握手堵塞时DNS可建立、client/STA generation恢复和旧回复丢弃。
- 4项production UDP wire测试通过：校验和/零checksum、长度/分片/options/端口拒绝、SOCKS头与容量、FRAG/RSV/ATYP拒绝。
- 原7项production TCP rewrite、11项TCP relay、512 mapping harness、DNS ID/RTT harness均通过。
- `prepare_lwip.py check`通过，项目override/全局IDF无额外改动。
- ESP-IDF v6.1编译成功，`build-udp07.log`；app786720 bytes（0xc0120），4MiB app分区剩81%。
- 独立默认配置reconfigure成功，`configure-udp07-defaults.log`与/tmp/esp-proxy-udp07-defaults-sdkconfig确认socket208、OOSEQ4、Octal PSRAM与16MB Flash。
- 烧录 `flash-udp07.log` hash校验通过。ELF SHA256 `ed6af9fab50de375fd3cfa234ee42039c9e3da477ae00e9d48760f0c29f553b9`，对应板上启动日志。
- UART `monitor-udp07.log`确认240MHz/双核、16MB Flash、8MB PSRAM初始化、AP+STA ready；STA仍192.168.1.103，AP客户端获取192.168.4.2。
- 初始窗口DNS=1/1/1、output=1，DNS RTT480ms、待发28976us；TCP一条正常关闭，首响应455ms。所有UDP drop计数0。这是基本板上通路验证，不是32UDP/64TCP压力测试，也不是APP性能对照。
- 初始网络ready internal_free234751 bytes、PSRAM_free6956580 bytes；随后空闲 internal233491、PSRAM约6.63MiB稳定。观察窗口无watchdog/crash，不能据此证明长期无泄漏。
- 启动时客户端先连信道6，STA随后关联信道13，AP客户端有reason2/209断开后重连并获得DHCP。该APSTA换信道过程仍存在，未把它描述为UDP控制连接故障。
- 上游JSON语法与当前版本字段/控制流核对完成；Windows合并/加载配置及Android交互改善尚待用户反馈。没有新吞吐量结果，不报告速度提升倍率。

复现：

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check
python tests/test_packet_rewrite.py -v
python tests/test_udp_packets.py -v
python tests/test_socks_relay.py -v
python tests/test_udp_relay.py -v
gcc -std=c11 -Wall -Wextra -Werror -I main tests/dns_flow_harness.c main/dns_flow.c -o tests/.out/dns_flow
tests/.out/dns_flow
gcc -std=c11 -Wall -Wextra -Werror -I main tests/flow_table_harness.c main/flow_table.c -o tests/.out/flow_table
tests/.out/flow_table
idf.py build
```

后续接通上游DNS并自然复测Android首页与视频；同时看association错误、DNS RTT/待发、TCP首响应指标。IPv4分片/大UDP/完整QUIC兼容和极限资源占用仍未验证。
