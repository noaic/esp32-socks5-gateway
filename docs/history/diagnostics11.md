# 第十一轮：ARP、驱动发送与 TCP 重传观测

第十轮回退后客户端测速7.11/6.35Mbps，UDP输出累计ERR_MEM=1、内部RAM仍有余量。本轮仅增加诊断：维持输入/回包队列、32/64KiB cache、TCP容量/压力回收、Wi-Fi buffers、UDP单次发送、DNS配置和固定任务，不恢复已退步的重试实验。

## 根据本机 v6.1 源码选择入口

`etharp.c:etharp_query`默认ARP_QUEUE_LEN=3，ARP尚未解析时queue满、queue条目或pbuf分配失败均可返回ERR_MEM。`wlanif.c:low_level_output`还可能在合并pbuf链时分配失败；进入`esp_netif_transmit_wrap`后驱动NO_MEM也映射为ERR_MEM。仅凭最终err_t和heap余量无法判断来源。

`esp_netif/include/esp_netif_net_stack.h`公开了`esp_netif_transmit_wrap`的实际四参数签名。wlanif传入完整连续Ethernet frame，由该API调用driver_transmit_wrap。本项目在链接阶段使用GNU --wrap观测这个外部调用，参数/返回值均原样转交原函数，没有修改全局IDF、本地lwIP源文件或netif函数指针。每接口记录调用数、NO_MEM、TX_FAILED和其他错误；不逐包打印、复制数据或增加动态内存。

UDP回包调用IPv4输出前后取AP的UDP驱动尝试计数。只有ERR_MEM时才查询现有ARP表：

- `before_driver ARP_missing`：UDP尚未交给驱动、客户端ARP未解析；支持问题位于ARP/驱动前，但不能仅凭此断言一定是三包queue满。
- `ARP_ready`：UDP尚未交给驱动、ARP已解析；考虑headroom、pbuf合并或其他前置分配。
- `driver_called`：UDP已交给驱动，结合AP driver mem/buf/other判断。ARP请求本身不计为UDP驱动尝试。

前后取样在同一TCPIP callback内，其他lwIP输出callback不能穿插执行。本项目没有其他task直接调用此driver API；若以后改变这个约束，必须重新审查计数归因。ERR_OK仍仅表示驱动/ARP接受，不证明空口送达。

TCP使用两处当前调用关系：`tcp.c:tcp_slowtmr`外部调用`tcp_rexmit_rto_commit`；`tcp_in.c`外部调用`tcp_rexmit_fast`。成功首次进入TF_INFR时才记录fast，未成功/已在恢复中不重复计数。wrapper仅读取PCB现有端口/flag，不写PCB或保存flow metadata。

日志分别记录本地listener端口15000的client侧及remote SOCKS端口7897的upstream侧。上游包括TCP CONNECT与UDP control TCP。计数是ESP32发送端的RTO定时器提交/快速恢复事件，不是重传segment数量，也不包含Android、Xray或远程服务器自己的重传。tcp_out.c内部SYN_RCVD恢复调用不会经由这个RTO wrapper，因此不将计数描述为全部TCP重传。

另外统计transparent输入/输出pbuf_clone分配失败，以免把应用rewrite的内存失败误归给无线。

补充诊断把RTO按handshake/established/closing分开。closing包括CLOSE_WAIT等半关闭状态，仍可能有有效待发数据，不据此将它们全部当成无关计数。TCP应用事件循环只在原10秒统计点扫描64个session，记录c2s/s2c待发流数和字节数、等待首次响应数及最久无进展的flow（上游本地端口/原目标/状态/待发量）。没有待发或等待首次响应的普通keep-alive不被算为blocked。两接口带宽使用本机v6.1的esp_wifi_get_bandwidth及WIFI_BW40读取，未调用set_bandwidth。

## 文件与验证

| 文件 | 内容 |
| --- | --- |
| `main/net_diagnostics.c/.h` | 包装函数、计数与每10秒汇总 |
| `main/CMakeLists.txt` | 加入组件源与3个链接wrap参数 |
| `main/app_main.c` | Wi-Fi启动前绑定现有AP/STA handle |
| `main/udp_gateway.c` | 仅在ERR_MEM时定位驱动前/后及ARP状态 |
| `main/tcp_poc.c` | clone失败计数、已有周期日志旁输出诊断 |
| `main/socks_relay.c` | 周期待发队列/最久等待flow与AP/STA配置带宽 |
| `tests/test_net_diagnostics.py` | 实际生产wrapper的主机调用契约回归 |
| `README.md`、本文 | 解释计数和保存验证结果 |

新增回归使用实际`main/net_diagnostics.c`和GNU --wrap；mock只代替SDK声明及原始API，核对原函数仅调用一次、相同data/len/pbuf/netif/PCB、报文未改变、任意错误值原样返回、短帧/NULL不越界、ARP不误计UDP、fast失败/已恢复不重复计数、无关接口/端口不归为代理业务及RTO状态分类。通过严格gcc -O2 -Wall -Wextra -Werror。TCP rewrite 7项、UDP报文4项回归通过。补充TCP队列观测后15项TCP socket回归重新通过（test-diag11-tcp.log），包括64并发/容量压力、8MiB echo、双向背压、EOF/RST和代际恢复；UDP socket核心未改，未声称重跑该集成。

ESP-IDF v6.1 build成功，无编译warning/error，app0xc11f0=791024bytes，分区余81%。ELF SHA256 `832ac71ce390985b8dfeff40caee6469cf2ac296cafabf40b1210ac8927ed205`。`build-diag11.log`保存构建记录。
补充版build-diag11b.log最终构建成功，app0xc1750=792400bytes，分区余81%；ELF SHA256 `6e0e0e677422405697a88f6904c01331bb99ebe817af230bde347ab5f5b1756b`。一次编译因误用旧枚举WIFI_BW_HT40失败，核对本机头文件后改成WIFI_BW40再构建，未烧录失败构建。
map交叉引用确认wlanif、tcp.c、tcp_in.c实际指向对应wrapper；反汇编同时确认wrapper调用原函数。补充版三个包装函数正文合计362bytes IRAM，另有literal/alignment；常驻计数及handle约112bytes RAM，无新的PSRAM池。统计仍有计数/日志开销，不能把“协议行为不变”等同于“性能绝对不受影响”，须客户端复测。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tests/test_net_diagnostics.py -v
python tests/test_packet_rewrite.py -v
python tests/test_udp_packets.py -v
python tests/test_socks_relay.py -v
python tools/prepare_lwip.py check
idf.py build
```

项目本地lwIP完整检查通过。包装依赖已核对的v6.1函数签名、调用位置和TF_INFR语义，升级IDF时须重审，而不是只观察链接通过；官方Wi-Fi实现是二进制，当前无法据这些计数直接给出空口重试次数。

## 板上验证

旧回退固件仍保留在本机忽略的`build-snapshots/udp10-baseline/`。首版flash-diag11.log hash验证成功，monitor-diag11.log的240秒采集完成；16MB Flash/8MB PSRAM、APSTA正常。用户反馈8.17Mbps下载、4.45Mbps上传，但首页视频列表刷不出来。新增诊断没有出现上一轮下载0.11Mbps那种退步，上传较回退的6.35Mbps下降，单次/混合负载不足以量化观测开销。

首版最终累计AP driver calls26842、NO_MEM42、other428，STA calls21683、所有driver错误0。client RTO97/fast0，upstream RTO186/fast3；UDP输入/输出/应用clone失败均0，DNS有一次10201ms响应、UDP443发送24无回复，3次DNS control TCP ECONNABORTED。没有STA断线/panic/watchdog。用户确认22:27:29的AP离开、6秒后重连是手动操作，移动数据关闭；因此不能把那次AP事件称为自动断线，离开前后“other”及累积重传也不能全部归为正在使用网页的丢包。首版没有RTO状态分类，不能判断186次里多少是close/FIN、多少是有效业务。

补充版flash-diag11b.log hash验证成功，monitor-diag11b.log的240秒采集完成。用户保持移动数据关闭，反馈“这次正常没卡住也没刷不出来”。这一版只补充诊断，没有改变转发、队列或无线参数，因此本次正常不能证明间歇故障已修复；未要求新一轮SpeedTest，不把首版8.17/4.45Mbps当成补充版测速。

| 补充版240秒窗口 | 观测结果 |
| --- | --- |
| AP driver | 34612次调用，NO_MEM 107，TX_FAILED及其他错误0 |
| STA driver | 27341次调用，错误0 |
| client侧TCP事件 | RTO 10，全部established；fast恢复9 |
| upstream侧TCP事件 | RTO 1，established；fast恢复3，包含UDP control TCP |
| DNS/UDP本地路径 | 输入队列满、输出错误、rewrite clone失败均0；UDP入117、发送117、回复/输出105 |
| TCP session | 周期样本峰值33/64，pressure_closed 0，末尾待发队列和awaiting_first均0 |
| 可用内存周期样本最低值 | internal 169235bytes，最大连续块124928bytes，PSRAM 6824372bytes |
| 运行异常 | 无STA断线、panic、watchdog；未出现最久无进展>=3秒的blocked日志 |

AP NO_MEM表明部分发送在驱动入口被拒绝，与总heap尚有余量并不矛盾；Wi-Fi内部发送资源另有限额。107/34612约0.31%是驱动调用失败占比，不能称为空口丢包率。这个窗口的RTO主要在client侧，上游很少；可将AP发送资源/客户端无线侧列为下一次卡顿时重点对照项，但本次没有复现卡顿，不能宣布它就是首页失败的根因。峰值33个session及未发生压力回收不支持这个窗口因64路容量耗尽而卡住。周期采样也不能排除样本之间短暂峰值。

UDP443发送18无回复，DNS发送67、回复71（可能含重复响应），无本地丢弃；这些计数不证明QUIC可用或每条DNS查询均成功。新日志保留原目标和上游端口，可在下次同一时间窗口出现故障时与Xray对应连接核对。

两接口日志的40/40MHz是`esp_wifi_get_bandwidth`读取的配置能力，不代表每帧或与上游路由协商后的实际PHY带宽。官方说明默认HT40、STA最终带宽取决于双方协商，并建议拥挤无线环境使用HT20；APSTA还共享无线信道。若后续重现客户端侧重传，可以单独比较HT20这一项，当前没有修改它。参考[ESP-IDF ESP32-S3 Wi-Fi协议说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-mac-protocols.html)。

补充版固件及校验记录保存于本机忽略目录`build-snapshots/diag11/`，旧回退版仍在`build-snapshots/udp10-baseline/`。快照含当前本机配置，不提交、不分享固件或ELF。

后续客户端自然使用即可，不需要脚本。再次卡住时先保持Wi-Fi连接、不手动重连或重启，以保留故障现场；对照ARP前置错误和驱动错误、两侧TCP状态/待发量及DNS等待。计数是累计32位无符号值，长时间运行可能回绕；取相邻样本增量时用无符号差。没有记录精确丢包率/RTT与cwnd分布，不因这些有限观测宣称根因全部解决。

## 再次卡住：22:56—23:00现场记录

用户报告首页又打不开后，22:56:42接入旧UART工具（带`--no-reset`）收到POWERON启动日志。用户确认没有插拔或按键，重启后首页一度恢复；因此重启前的故障没有采到。检查本机pyserial实现，打开时仍顺序设置DTR/RTS并清空输入，与保留故障现场的意图冲突。修改仅涉及主机采集工具，不改固件或烧录：新增`tools/uart_passive.py`，POSIX被动接入不设置modem线、不flush输入、关闭HUPCL；支持`--seconds 0`保持端口打开。PTY检查通过：待收日志保留、超时读、独占锁、无modem ioctl/flush。首次打开时驱动或适配器仍可能改变控制线，不把软件参数视作硬件绝对保证。

`monitor-diag11b-stall.log`保存随后240秒采集。用户明确反馈从22:58开始再次刷新无效，直到Android自动断开Wi-Fi；本轮22:59:35的离开不是手动操作，与此前首版手动重连不同。

| 时间（CST） | 观测 |
| --- | --- |
| 22:57:03 | DNS输入队列满丢弃7个请求；新接入突发时UDP32/32，后续逐渐下降；不是本地UDP输出失败 |
| 22:57:14 | 一条120.92.218.109:443连接等待首响应约25秒；其他连接仍有响应，不认定为首页目标 |
| 22:58:14 | 一条39.136.93.66:443回包待发2976bytes，无进展7.3秒 |
| 22:58:24 | 3条s2c待发，共10464bytes；222.210.39.10:443的flow154待发4416bytes |
| 22:58:34—22:59:35 | 上述3条待发量不变；flow154无进展从15秒增至75.8秒；转发累计字节也保持不变 |
| 22:58:38—22:58:50 | 一个UDP443及两个DNS association控制连接报ECONNABORTED（本机errno113） |
| 22:59:31 | 重连前client RTO established17/closing42；upstream handshake9/established10/closing87；AP NO_MEM51，STA driver错误0 |
| 22:59:35 | Android离开AP，reason3，应用按已有逻辑清理client相关连接 |
| 22:59:42 | Android自动重连；离开期间出现AP错误0x3015，即本机ESP_ERR_WIFI_NOT_ASSOC，不能倒推为之前已关联时的故障原因 |
| 22:59:55 | 待发量及awaiting_first均0，转发字节继续增长 |

本窗口TCP周期峰值55/64，未触发pressure回收；最低internal158767bytes、最大连续块114688bytes、PSRAM6781112bytes；无STA断线、panic或watchdog。这个证据支持“发生过实际数据转发停滞”，也说明不能只把问题归为DNS：已有TCP连接已收上游数据却未继续送回客户端。DNS突发丢弃确实存在，可以贡献重试延迟，但是否导致这次持续卡住尚不清楚。没有触发应用64路上限，也不等于排除更底层PCB/资源上限。

阅读flow_table_expire确认socket_open的映射不会被超时回收；本轮没有改flow表。现有日志仍不足以区分客户端零窗口/ACK中断、重写校验/映射拒绝、ARP或驱动问题；后续若加诊断，应在TCPIP线程只读地采样对应PCB的发送队列/窗口，以及逐flow输入输出与拒绝计数，避免在别的task直接遍历PCB。

23:01:08改用被动工具持续采集，保存`monitor-diag11b-live.log`（无时限）。接入后板子时间戳保持约263秒并继续增长，没有启动横幅，说明本次接入没有再次复位；最初残留串口片段可能不完整，故分析使用后续完整行。不要同时启动第二个monitor，也不要为了取日志反复关闭/打开端口。
