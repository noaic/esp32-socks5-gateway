# 全项目审查与第四轮性能修改

## 范围与结论

阅读全部 main 模块、项目 Kconfig/CMake/defaults、IPv4 patch/应用脚本和已有测试，
并核对本机 ESP-IDF v6.1 的 tcp_out.c、ip4.c、sockets.c、err.c、lwIP Kconfig、
VFS eventfd 实现与官方 eventfd/iperf 示例。没有引入其他网络协议、客户端或管理面。

最值得先处理的是多余的出包 checksum 扫描、UDP 队列唤醒延迟和 TCP 调度周转。
仅提升活动连接数量无法解释单流速度，也不能解决两端互相关闭连接的问题。
上一轮用户报告361K/22秒（更早249K/32秒）；此次用户报告500K/15秒，详细限制见下文。

## 审查发现

| 优先级 | 位置 | 发现及处理 |
| --- | --- | --- |
| 高：延迟 | udp_gateway.c 输入队列/worker select | xQueueSend不能唤醒socket select，空闲时新增请求可多等接近100ms；使用本机官方VFS eventfd唤醒，保持一个固定UDP worker |
| 高：关闭正确性 | socks_relay.c shutdown路径 | 本机lwIP ERR_CLSD/ERR_CONN映射为ENOTCONN，写半边已关闭时原代码会abort另一方向；现在继续排空接收数据，其他RST/recv/send错误仍正常终止 |
| 中：CPU开销 | tcp_poc.c/packet_rewrite.c 输出 | 本机tcp_out.c在ip_output_if前生成TCP checksum，ip4.c在hook前生成IP checksum；原输出路径又完整扫描payload校验，重复工作；改为40字节结构/IP检查及字段增量更新 |
| 中：调度开销 | socks_relay.c relay路径 | 新读到的队列数据在下轮select才发送；现在同一轮尝试一次非阻塞send，EAGAIN保留队列，避免无界循环或饿死其他flow |
| 中：窗口 | sdkconfig.defaults | TCP RX window11520偏小；调到23040，TX17280，RX mailbox18（window/MSS+2），accept mailbox6→16缓解连接突发 |
| 中：日志 | UDP map/first reply | 逐条DNS流INFO日志占UART和日志锁；降为DEBUG，以10秒汇总dispatch/queue wait替代，association错误仍INFO/WARN |
| 待实测 | pbuf clone/PSRAM/无线 | clone仍每包复制和分配；AP+STA共享信道；未取得可量化这两者占比的硬件profile与LAN baseline |

UDP恢复时清空flow/待发队列仍有数据丢失窗口；eventfd不能修复Xray关闭UDP控制连接。
队列仍固定8个datagram，UDP flow仍32，超过容量仍按原策略丢弃；QUIC/fragmentation未扩展。
TCP 120秒idle、15秒SOCKS握手超时保持；闲置连接日志不等于网页失败。

## 修改文件和架构

- main/packet_rewrite.c/.h：增加仅供本机lwIP输出的header检查；外部入包仍严格校验TCP/IP。
- main/tcp_poc.c：输出先检查40字节头，再clone并增量重写。编译检查要求
  CHECKSUM_GEN_TCP/IP开启、per-netif checksum关闭；防止切换硬件checksum配置后错误复用。
  保留私有clone，避免改坏TCP重传pbuf或ARP缓存持有的引用。
- main/socks_relay.c：fresh队列即刻send；ENOTCONN半关闭处理；保持64路、双方向8192字节
  固定PSRAM缓冲、单event loop、EOF/部分写/背压、线程安全network epoch。
- main/udp_gateway.c：单eventfd唤醒混合VFS/socket select；队列入包和网络代际变化通知；
  eventfd只负责通知，FreeRTOS队列持有数据；合并通知不会合并datagram。
  使用本机API eventfd(0,0)，未猜测Linux EFD_NONBLOCK（本机v6.1不接受该flag）。
  分配失败路径关闭eventfd、注销VFS、释放启动分配；不新增task。
- main/CMakeLists.txt：显式添加vfs依赖。
- sdkconfig.defaults及忽略的sdkconfig：适度增大TCP窗口/mailbox；固化混合select要求。
- tests/test_packet_rewrite.py：40字节实际buffer/大packet/TCP options/输出增量checksum、
  输入corrupt payload仍拒绝。
- tests/test_socks_relay.py、tests/relay_harness.c、tests/shutdown_closed_shim.c：
  增加双向背压和已关闭写半边保留响应回归；测试shim只链接进host executable。
- README.md、本记录：更新验证和后续测量。
- .gitignore：忽略独立defaults验证产生的build-*目录。
- tools/probe_upstream_udp.py：开发机只读SOCKS5 UDP DNS对照工具，不加入firmware。

flow_table.c/.h 的full 4-tuple哈希、内部端口标记、512条关闭保护期和2*MSL/quiet guard保持。
原IPv4 hook patch保持；prepare_lwip.py检查完整项目override及全局IDF未修改通过。
AP+STA事件/DHCP DNS仍1.1.1.1、NO AUTH IPv4 SOCKS5配置和GPIO48预留保持。

## 配置预算

未照抄官方单流iperf的65535窗口。64 TCP relay意味着最多128个TCP socket：
只按每socket窗口/发送缓冲的逻辑容量估算，23040+17280乘128约4.92MiB，
加固定relay约1MiB；这不包含pbuf/PCB/mailbox/UDP/Wi-Fi等开销，也不是物理内存峰值保证。
缓冲按实际数据动态占用，仍需满负载内存检查。当前176 socket、FD_SETSIZE256保持。
Wi-Fi RX/TX/AMPDU、20MHz、channel、国家码和CPU/task affinity没有同时改动，
以免把空口变化与代码改进混为一谈；watchdog保持开启。
IRAM优化Kconfig明确对双核收益有限并消耗额外IRAM，此轮未盲目启用。

## 验证

- 7项production packet rewrite测试通过。
- 11项production TCP event loop回归通过：64并发、65th等待/恢复、8MiB精确echo、
  分段/合并SOCKS reply、错误/重连、双向1MiB请求+2MiB响应、缓慢接收/小发送缓冲、
  模拟lwIP ENOTCONN后1MiB响应完整且无reset。
- production flow table严格C harness通过512条source-port隔离/满容量/过期保护/hash复用。
- ESP-IDFv6.1 build成功，无warning/error，镜像0xbf310，4MiB app分区余81%。
- flash-review04.log hash校验通过；monitor-review04.log确认240MHz、16MB flash、
  8MB PSRAM memory test、AP+STA DHCP和UDP ASSOCIATE ready。
- 联网启动快照internal_free=234475、PSRAM_free=7178888 bytes。
  这是空闲启动验证；没有据此宣称满载无泄漏、长期稳定或已经提升多少带宽。
- 独立默认配置reconfigure通过：configure-review-defaults.log及/tmp/esp-proxy-review-sdkconfig
  确认16MB flash、Octal PSRAM、176socket及新窗口/mailbox，不依赖原sdkconfig的隐藏配置。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check
python tests/test_packet_rewrite.py -v
python tests/test_socks_relay.py -v
gcc -std=c11 -Wall -Wextra -Werror -I main tests/flow_table_harness.c main/flow_table.c -o tests/.out/flow_table
tests/.out/flow_table
idf.py build
```

板上eventfd与socket混合select已完成初始化/STA事件唤醒及association握手。
AP客户端datagram、queue_wait指标和下载已获本轮板上验证，见下文；host测试不覆盖ESP VFS实现。

## 客户端本轮反馈与待解决DNS

用户报告500K/15秒，上一轮361K/22秒；速度约增加38.5%、耗时约减少31.8%。
仍是单次用户报告，不是严格的多次平均，也没有将收益归因到某一个修改。
monitor-review04.log可见8MiB下载flow40正常关闭reset=0、upstream_bytes=8389932
（包含HTTP头部），没有payload中途失败证据。

实际UDP调度窗口记录：30请求avg3044/max12932us，随后8请求avg1537/max2567us；
后续少量请求平均约711~1422us。官方eventfd唤醒已处理AP客户端datagram，
这些数据验证队列排队较短，尚不等于DNS请求往返成功。

用户同时报告nslookup超时。控制TCP在15:06:30报control_exception errno113，
随后3次CONNECTING state1握手超时，直到15:07:40恢复association。
必须使用ESP实际工具链errno.h：ECONNABORTED=113、EHOSTUNREACH=118、
ENOTCONN=128、ETIMEDOUT=116、ECONNRESET=104，不能套Linux编号。
113是本地连接中止，可能来自lwIP keepalive无应答后的abort；没有抓包前不能确定具体触发。
不能直接认定为公网DNS服务故障，或eventfd造成连接中断。

同一时期WSL直接通过相同SOCKS5获得HTTP301，tools/probe_upstream_udp.py
得到1.1.1.1 DNS rcode0/answers2/491.2ms。这是绕过ESP32的上游对照，
不证明此前ESP32到Xray链路没有中断，也不能替代AP客户端DNS验收。
客户端后续确认恢复：实际查询google.com，Server显示one.one.one.one，
返回地址142.251.40.110。说明此时透明UDP/DNS回包路径工作；
仍不能宣称之前中断的触发已修复，或长期不再出现DNS超时。
后续15:16:07又报ECONNABORTED，多个CONNECTING超时，15:17:40再次恢复。
STA未出现disconnect，不能仅凭RSSI判断STA与Xray之间的TCP链路健康。
已请求上游Windows的`arp -a 192.168.1.103`，核对STA MAC
90-70-69-f8-f1-08；用户返回动态记录的MAC完全匹配，未发现当前邻居记录错误。
这是当前时刻的检查，不能据此排除故障时刻的所有网络问题。
下一步需要在Xray所在Windows捕获ESP32对应TCP流，确认keepalive ACK/SYN应答
是否到达；WSL NAT路径无法替代这段LAN链路的抓包。

最终build-review04-final.log成功，ELF SHA256前缀83d674c0f与板上启动日志一致；
源码性能版本与当前固件一致。没有因DNS瞬时故障反复调整协议/窗口或更换DNS。

## 最短客户端复测

Windows连接ESP32 AP，关闭浏览器，执行同一个下载URL：

```cmd
curl.exe -4 -o NUL "http://speed.cloudflare.com/__down?bytes=8388608"
nslookup example.com
```

先比较单流耗时/速度和DNS，再观察多标签；不要同时播放视频测速。
UDP dispatches/queue_wait_avg_us/max_us用于验证队列唤醒；它包含任务调度和association
未ready的等待，不等于DNS网络RTT。无客户端结果前不承诺倍率。
进一步定位需要同条件的AP+STA LAN baseline与上游对照；WSL在上游NAT网络，
不能把WSL直连ESP AP客户端或在WSL直测上游当作透明路径的完整验收。
