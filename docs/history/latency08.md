# 第八轮：AP 接入与连接延迟

## 现场证据

用户修改 Xray DNS 配置后报告无法连接 AP，随后确认已连接、体验改善但仍有卡顿。
`monitor-ap08.log` 的 20:43～20:46 窗口记录：

- `02:00:00:00:00:01` 已关联，DHCP 分配 `192.168.4.2`；随后两轮 `max connection, deauth!`。唯一 AP 名额被占用时其他关联会被拒绝；被拒绝设备的 MAC 未在日志中给出，不能确定具体是哪台设备。AP 上限保持1，建议其他测试设备关闭自动连接。
- STA 保持联网，RSSI约 -32～-41 dBm；未见 STA 断线、崩溃或 watchdog。此结论仅限该采集窗口。
- TCP 活动峰值43，小于64；存在客户端侧和上游侧 `errno=104`。上游 socket 错误不能单独区分 Xray、远端目标和传输链路故障。
- DNS 有正常回复，各10秒统计窗口 RTT平均约8～126ms；UDP全类别待发最长483534us。该旧指标包含新association握手，不是纯任务调度延迟，也不能将两个窗口平均值相加当作同一查询的耗时。
- UDP/443输入6、发送6、回复0。可能引起 QUIC 尝试后的回退等待，尚未证实是所有卡顿的原因。本轮没有封禁UDP/443或实现新分流规则。
- 从 WSL 绕过 ESP32，通过现有 SOCKS5 下载1MiB：connect0.000609s、first byte0.543053s、total1.427627s、734488B/s。这是单次上游比较，不是AP性能、不是与此前8MiB测速等价的对照；WSL源地址不同也不能验证仅匹配ESP32源IP的DNS路由。

## 实现

1. 已完成 DNS 的空闲保留从5秒改为30秒，配置集中到 `PROXY_UDP_DNS_IDLE_SECONDS`。同一原始四元组短暂停顿后可复用 association，减少重复握手和control TCP的关闭/重建。只改变已完成查询的TTL：等待匹配回复的DNS仍使用普通60秒TTL；满表仍优先回收已完成且无待发数据的DNS，最多32flow与DNS预留保持。
2. TCP保留120秒空闲阈值。在RELAY状态、两个应用待发队列均为空、无已报告socket错误时，空闲到期使用shutdown写半部和close正常结束。握手超时、网络代际改变、真实socket错误，以及有待发数据的堵塞超时仍使用RST。正常空闲回收单独计数 `idle_closed`，不再当作reset。这可减少固件主动RST对持久连接的影响，不能保证浏览器绝不发生连接复用竞态。
3. UDP统计增加 DNS 专属待发耗时、warm待发耗时、成功ASSOCIATE握手耗时。warm指worker收包时该association已ready、之后直到发出未发生association错误；它仍包含输入队列/发送重试时间。ASSOC从本机发起非阻塞connect到接受有效reply并连接UDPrelay，包含LAN连接和协议握手，不包含外网DNS查询。DNS RTT仍从实际发送至匹配回复提交，未测到客户端实际接收。
4. 每10秒输出当前AP客户端数与MAC，让单客户端名额占用可以直接核对。

单UDP/task、单TCP/task、独立flow table、IPv4 rewrite与lwIP职责保持；无全局ESP-IDF修改。没有调整TCP窗口、Wi-Fi buffer、AP客户端上限或CPU参数。
官方v6.1说明缓冲/窗口与内存预算相互制约，多流应缩减单流预算，PSRAM模式采用static TX：[Wi-Fi性能](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)。目前以证据明确的连接生命周期改进为先，不以单流iperf实验室结果作为本项目APSTA吞吐保证。

## 文件与验证

- `main/proxy_config.h`：已完成DNS TTL。
- `main/udp_relay.c/.h`、`main/udp_gateway.c`：生命周期和分段统计。
- `main/socks_relay.c`：空闲FIN回收与AP占用诊断。
- `tests/test_udp_relay.py`：新增已完成查询复用/到期、未答查询不误按短TTL回收。
- `tests/test_socks_relay.py`、`tests/shutdown_closed_shim.c`：新增全开持久连接两端FIN、半关闭空闲FIN、有未发数据仍RST；send shim只在测试中模拟确定性的EAGAIN。
- `README.md`、本文：记录行为与限制。

生产UDP核心13项socket集成测试通过；生产TCP核心13项通过，含8MiB echo、双向背压、断流恢复等原有回归。
`prepare_lwip.py --idf-path /home/user/.espressif/v6.1/esp-idf check`通过。
ESP-IDF v6.1 `idf.py build`成功，`build-latency08.log`；app大小0xc07c0（788416bytes），4MiB分区剩81%。
ELF SHA256：`f218d9ba052d503e9ddeab8b087b51e1877bfdce0bbdc743d9152227eff01b82`。
构建有两条remote Wi-Fi派生默认值提示，未修改这些参数；当前板上本地Wi-Fi日志确认RX buffer16/48、RX BA16，不能把remote配置的6解释为该板生效的RX BA。

复现命令：

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tests/test_socks_relay.py -v
python tests/test_udp_relay.py -v
python tools/prepare_lwip.py check
idf.py build
```

板上交互验证使用 Android 自然刷新首页、加载封面、播放/拖动视频；不要求复制脚本。吞吐改进幅度、QUIC行为和长时间稳定性尚未验证。

## 烧录与本轮交互结果

`flash-latency08.log`烧录完成，hash校验通过。`monitor-latency08.log`启动ELF标识为`f218d9ba0...`，确认16MiB Flash、8MiB PSRAM、AP+STA ready和Android DHCP192.168.4.2。
用户按相同APP/网页交互复测反馈：“好多了，只不过稍微有些首页刷新延迟而已”。这是用户体验反馈，不能将其归因于某个单一改动或等同于带宽提升。

运行统计证实：

- DNS_WAIT可分离冷启动成本。例如21:00:12窗口待发平均160583us，成功ASSOC握手平均119058us，DNS发送后RTT平均46ms。另一个高负载窗口DNS待发平均473568us、最大608075us，成功ASSOC握手平均207327us、最大466520us。两个平均值的样本集不同，不能精确相减计算纯调度时间。
- 已ready的warm待发在若干有样本窗口平均约11～24ms，仍有更长峰值；许多DNS查询使用新源端口，新flow必须进行独立握手。延长TTL对同四元组复用有帮助，对这些全新flow不能消除握手。
- 最多32UDPflow，完成DNS满表回收累计24次，未出现队列、flow容量、association、发送/回包等UDP丢弃计数。完成DNS延期保留没有妨碍压力回收。
- TCP活动量上升到55/64；21:02:04起实际出现`idle close`，验证板上新分支运行。客户端RST与少量上游RST仍存在，没有把它们描述为已完全解决。
- 初始ready internal234831bytes、PSRAM6957728bytes；活动中已观察internal207287bytes（约202KiB）、最大连续块131072bytes，保留足够余量；这不是64TCP+32UDP极限压力或长期泄漏验证。
- UDP/443已发送47包仍无回复，QUIC回退等待仍是剩余延迟的候选原因；其他UDP/DNS通路正常，不能据此断言整个上游UDP出口不可用。

后续保留当前稳定基线；若专门继续优化首页首开，优先比较DNS冷握手与应用首响应，再评估有界预建立association等方案。不能把已使用的association直接转给新客户端源端口：相同远端和相同DNS ID的迟到/重复回复需要可靠隔离。不要为省握手恢复有串流风险的单association多源端口方案。

完整180秒采集结束：STA断线0、AP满员拒绝0、TCP活动峰值55、空闲正常关闭事件15次；所有UDP drop计数0。最低internal207287bytes，最低PSRAM6860360bytes（约6.54MiB）。最终UDP/44348/48/0；DNS原始回复85大于发送80，计数包括重复回复，不能把这个比例当作查询成功率。未见panic/watchdog。
