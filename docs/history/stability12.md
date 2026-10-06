# 第十二轮：手机断流与 Wi-Fi 聚合兼容性

用户要求尝试解决23:00前后反复出现的“AP仍连接、首页刷新失效、手机自动离开AP”问题。本轮保留透明改写、独立flow表、TCP/UDP事件循环、DNS和队列路径；lwIP继续负责TCP状态，不强改窗口、不强制重传、不用周期踢手机掩盖停滞。

## 本机源码与官方资料

- 活跃mapping的`socket_open`为true时，`flow_table_expire`不会回收它。没有发现这次停滞因活跃映射过早清理而发生。
- 上游启动日志协商BW20，旧SoftAP客户端接入为40D。先用本机v6.1的`esp_wifi_set_bandwidth(..., WIFI_BW20)`固定AP/STA为HT20，调用在init、set_mode/config之后且start之前。官方建议拥挤无线环境使用HT20，但这只是可逆的兼容性试验：[Wi-Fi MAC协议](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-guides/wifi-driver/wifi-mac-protocols.html)。
- [官方ESP-FAQ](https://docs.espressif.com/projects/esp-faq/en/latest/software-framework/wifi.html)针对S2/S3 SoftAP与手机/电脑兼容性给出先禁用AMPDU RX、仍断开再禁用TX的排查顺序，并说明会影响吞吐。本机`components/esp_wifi/Kconfig`对应`ESP_WIFI_AMPDU_RX_ENABLED`和`ESP_WIFI_AMPDU_TX_ENABLED`，未使用旧别名。
- 官方仓库[问题18882](https://github.com/espressif/esp-idf/issues/18882)报告过SoftAP省电客户端的单播缓存/TIM异常、重连恢复。它是其他环境的未解决报告，不是本项目根因证据，也没有据此修改二进制Wi-Fi驱动或采用其强制TCP重传做法。

## 修改文件与行为

| 文件 | 修改 |
| --- | --- |
| `main/app_main.c` | 12a—12c兼容试验全部失败，12d已恢复默认b/g/n和协商带宽；打印初始化AMPDU RX/TX开关 |
| `sdkconfig.defaults`、本机忽略的`sdkconfig` | 12b关闭RX试验退步，12c恢复RX和BA16；TX一直开启 |
| `main/tcp_poc.c/.h` | 累积改写成功/校验/映射/shape/netif错误计数；阻塞连接PCB/ARP只读快照 |
| `main/socks_relay.c` | 在原10秒日志发现>=3秒无进展连接时请求一次快照；转发和select规则不变 |
| `README.md`、本文 | 参数、验证与回退记录 |

快照通过`tcpip_try_callback`在TCPIP线程读取active PCB及ARP；只把mapping副本带出短临界区，未保存PCB指针或写PCB。一个固定request和原子busy标志防止重复排队，无新增task、动态池或每连接任务。记录client/upstream的snd_buf、snd_wnd、cwnd、unsent/unacked字节、queue length、rcv_wnd、lastack/snd_nxt、nrtx/rto/persist，以及mapping包计数和ARP MAC。关闭/退休的flow可能在callback执行前改变，日志只是当时快照，不能视作跨线程原子事务。

仅输出最久无进展的连接；它未必是首页API。日志也不是空口抓包，不能凭零窗口断言Android APP有bug。统计和偶发快照打印有开销，未宣称完全没有性能影响。

## 构建和回归

12a HT20 app为795408bytes（0xc2310），ELF SHA256 `1147e0d729218aa3b72ff32c35e53604cf1bdf60860483e1f4ef05872d2122de`。12b RX-off app同尺寸，ELF SHA256 `78c18b6e0f9536964559892a6fcf978908c5a801ab09fb5fa74bc7d7f6c36e7f`。均ESP-IDF v6.1 build成功，无编译warning/error，4MiB应用分区剩81%；flash日志记录写入hash校验成功。

12c AP b/g版app796832bytes（0xc28a0），ELF SHA256 `9448eacef6c65b007e14212c448d61a887941514525f08383898dfb91be7553d`；build-stability12c.log构建成功，无编译warning/error，flash-stability12c.log记录hash校验成功。硬件和应用分区不变。

新增PCB诊断前后，15项实际生产TCP事件循环回归通过，覆盖64并发、8MiB数据、双向背压、EOF/RST和网络代际恢复；7项rewrite/flow回归、1项真实链接wrapper契约回归、4项UDP wire回归通过。项目本地lwIP patch完整校验通过，全局IDF未改。12b只有Wi-Fi配置/启动日志变化，未重复相同主机协议回归；主机测试不模拟Wi-Fi或硬件TCPIP callback。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check --idf-path /home/user/.espressif/v6.1/esp-idf
python tests/test_socks_relay.py -v
python tests/test_packet_rewrite.py -v
python tests/test_net_diagnostics.py -v
python tests/test_udp_packets.py -v
idf.py build
```

日志：`build-stability12.log`、`build-stability12b.log`、`test-stability12-*.log`、`flash-stability12*.log`。

## 12a 板上结果：HT20没有解决

`monitor-stability12-live.log`确认AP/STA配置20/20MHz、Android以bgn20接入、DHCP192.168.4.2。用户反馈23:13又卡住。

23:12:52—23:13:13，flow91（39.136.93.95:443）持续pending_s2c4416bytes；PCB client snd_wnd0、unsent16384、unacked0、snd_buf896、persist3→5。上游snd_wnd63798、unsent/unacked0；upstream rcv_wnd0是本地不再读取引起的背压，不能把它当作Xray对ESP发送的零窗口。ARP正确为Android MAC。mapping_open1，rx_packets127不再增加，tx_attempts383→385说明lwIP仍发persist探测。

23:13:02—23:13:13全局3条s2c待发9211bytes保持不变，active52/64，首次响应及转发累计停止增长。该区间input_ok4040不变，输入/输出parse、mapping及shape拒绝均0，既有netif失败21不再增加，DNS输入队列丢弃0。若干DNS/UDP control报本机ECONNABORTED，但没有证据证明全部是Xray主动断流。

23:13:20手机离开AP（reason1），23:13:21重新连接、随后转发继续。因此HT20单独试验失败，不能把此版本称作已解决问题。零窗口可能是正常暂停读取；全局输入停滞及手机重新关联仍需对照无线兼容性。没有通过修改TCP窗口或缩短timeout去“修复”零窗口。

## 12b 板上结果：全局RX关闭退步，已回退

12b只进一步关闭AMPDU RX，保持HT20、AMPDU TX和全部转发参数。用户反馈从接入就卡，首页图3—5秒或不加载，刷新更慢。日志`monitor-stability12b-live.log`记录client零窗口再次持续，23:23:15手机离开AP；23:24时AP NO_MEM191、STA NO_MEM274，ARP仍正确，输入/输出parse和mapping拒绝0，DNS输入队列丢弃0。NO_MEM增多与体感退步同时发生，不把计数当作精确空口丢包率或唯一因果。

12b没有解决问题，配置已回退：恢复`CONFIG_ESP_WIFI_AMPDU_RX_ENABLED=y`、RX BA16。没有在这个退步基础上继续全局关闭TX。用户确认Android无VPN，私人DNS默认/自动。

## 12c：只限制AP为b/g

本机v6.1公开`esp_wifi_set_protocol(WIFI_IF_AP, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G)`，在Wi-Fi start前调用。AP不再协商802.11n及其AMPDU，STA仍使用默认b/g/n及恢复后的RX/TX聚合。此方案保留上游聚合性能，针对手机这一侧做兼容性比较；AP最高PHY速率下降，不能称为吞吐优化。保留HT20，既有TCP/UDP队列和timeout不变。

23:28:14 Android以bg20接入，DHCP正常。用户反馈23:29又卡、随后首页图全部无法刷新且比以前更差。此试验失败，12d已撤回，不能称作修复。日志`monitor-stability12c-live.log`已经保存；每次烧录后串口初始段可能含旧速率残留乱码，分析使用后续完整行。

23:29:44—23:30:34，flow55（150.138.104.1:443）client snd_wnd65535保持非零，但1440bytes持续unacked，unsent12960、lastack不动，nrtx2→5/rto16→128。此时ARP正确、mapping_open1，改写parse/mapping/shape拒绝为0。这证明上一轮看到的零窗口不能解释所有卡顿；存在接收窗口开放但收不到TCP ACK的连接。全局AP输入计数停止增长，重连之后恢复。不能仅凭ESP_OK声称帧已在空口成功发送，或凭这些快照确认Android/Xray/Wi-Fi驱动根因。

客户端测试无需命令：移动数据关闭，连接AP，刷新B站首页、播放/拖动视频连续两三分钟；若卡住先保持连接并报告时间。吞吐对比等稳定性验证后再做。

## 回退

本机忽略目录分别保存`build-snapshots/diag11/`（旧HT40聚合开启版）、`build-snapshots/stability12-ht20/`、`build-snapshots/stability12b-rx-off/`和`build-snapshots/stability12c-ap-bg/`，每份manifest给出烧录offset及SHA256。快照含本机配置，不提交或分享。

12d已恢复RX=y、RX BA16，移除AP set_protocol和两处set_bandwidth，使用IDF默认b/g/n和协商带宽。`build-stability12d-rollback.log`构建通过，app794304bytes，ELF SHA256 `3fdc76faddc16a695a2ab423819630cc30420d85838cbfc8b1943ad2127140e0`，`flash-stability12d-rollback.log`烧录hash校验成功。快照`build-snapshots/stability12d-rollback/`保存该配置，保留PCB只读诊断。串口`monitor-stability12d-rollback-live.log`确认Android已接入、业务转发继续、带宽配置恢复40/40MHz；该API报告配置带宽，不能据此断言每个peer实际协商40MHz。全局IDF和lwIP patch无需恢复操作。


## 12d 使用反馈与 12e 待用诊断

用户确认12d回退后“恢复并且正常能刷视频”。保留12d在板上运行，不为追加诊断立即重启正在正常使用的客户端。该反馈证明撤回退步参数后的当前使用恢复，尚不能证明此前周期性断流已消失，也不能将一次重启后的恢复归因于确定的根因修复。

12e源代码及构建产物准备完成，**尚未烧录或做板上验证**。它沿用12d无线参数、透明改写、TCP/UDP事件循环和队列，仅增加以下观察点：

- `main/net_diagnostics.c/.h`：链接包装`esp_netif_receive`，每接口累计进入驱动收包入口的调用/字节/拒绝和最近时间。原参数和返回值保持不变；真正接收路径取得RX buffer所有权，包装函数不保留、复制或额外释放它。
- 同文件：链接包装`esp_netif_netstack_buf_ref/free`，记录跨AP/STA总引用/释放和暂时持有的引用数。继续调用原引用/释放函数，未替换Wi-Fi默认注册的buffer callbacks。计数仅表示驱动经这些接口持有的引用，不能把正常短时非零当作泄漏。
- `app_main.c`：在Wi-Fi start后调用本机v6.1私有`esp_wifi_set_tx_done_cb`安装完成回调；注册失败仅记录warning。当前项目没有其他TX-done订阅者。回调不打印、不分配、不发送数据，只读取接口、状态、802.11 FC及receiver address，统计单播data成功/失败；忽略beacon、多播和Null/QoS-Null，保留总callback及短帧数。没有保存或修改frame/length指针。该私有API并非稳定兼容承诺，升级IDF需复核。
- `main/CMakeLists.txt`：追加三个API的GNU `--wrap`；`tests/test_net_diagnostics.py`：真实生产wrapper在主机上执行，核对RX参数/返回值与buffer所有权消费、reference/free原函数调用、NULL、callback接口/短帧/多播/beacon/Null过滤、数据和length不变，以及既有重传观察契约。

本机源码`esp_wifi/src/wifi_netif.c`确认开启SPIRAM_TRY_ALLOCATE_WIFI_LWIP时使用tx_by_ref；`esp_private/wifi.h`约定它先增加引用、由驱动之后释放。当前output独立clone、调用output后释放应用自己的引用符合这个约定，尚未发现提前释放证据。没有把by_ref改成copy发送，也没有禁用PSRAM来试错。

`wifi_done`是驱动的完成通知，不是独立空口抓包；它不能单独证明TCP payload已经被APP消费。先在正常窗口确认callback/单播分类有有效计数，再对照卡顿窗口的AP RX、单播completion、pbuf held、PCB和driver拒绝，避免把空计数或正常安静时段当作驱动故障。32位微秒age允许约71分钟以内的差值，极长安静时段会回绕；bytes/packet计数也会回绕。log字段是独立原子采样，非跨计数一致事务。

12e `build-stability12e-wifi-observer.log` build成功，无编译warning/error，app795648bytes，4MiB应用分区剩81%。ELF SHA256 `7cbad26ea192cf58413acfb8476061143388ad2d025ba660525d24045f936d2c`。`test-stability12e-wifi-observer.log` wrapper契约测试通过；`test-stability12e-lwip.log`项目本地patch验证通过，全局IDF未改。nm确认新wrapper、TX callback和timer位于IRAM。未再次运行未改动的TCP/UDP协议回归；本轮前面的协议回归结果不代表Wi-Fi私有回调已经得到硬件验证。

12d正在运行的快照：`build-snapshots/stability12d-rollback/`；12e待验证快照：`build-snapshots/stability12e-wifi-observer/`。当前源码和build目录对应12e，后续执行idf.py flash会烧录12e，不是12d。若再次复现，先保留`monitor-stability12d-rollback-live.log`现场，再考虑12e观测；用户当前无需额外命令或测速。

## 2026-10-03：恢复项目调试，12e 已烧录

无线网卡排查已停止，未因此改动 ESP32 项目代码。本次重新核对并使用
已准备好的 12e，未改变无线带宽、聚合、队列、连接数、DNS 或转发算法。
前文“12e 尚未烧录”描述的是 10 月 2 日状态，现在板子运行 12e。

- `build-resume13-observer.log`：构建成功，无 warning/error，app/ELF hash
  与 12e 快照相同，app 795648 bytes。
- `flash-resume13-observer.log`：写入校验通过。
- `test-resume13-tcp.log`：15 项 TCP 回归通过。
- `test-resume13-udp.log`：14 项 UDP 事件循环回归通过。
- 另执行 1 项诊断 wrapper、7 项 packet rewrite、4 项 UDP wire 测试，均通过；
  项目本地 lwIP patch 校验通过。
- 初次在沙箱内执行 TCP/UDP 回归因禁止创建 socket 而失败，随后在沙箱外
  重跑上述测试通过。主机回归不验证真实 Wi-Fi 驱动行为。
- `monitor-resume13-baseline.log`：烧录前无 AP 客户端，内部空闲 185703 bytes，
  STA RSSI 约 -30 dBm，channel 3。该空闲基线不能证明带载时无问题。
- `monitor-resume13-observer-live.log`：新固件被动连续采集，完整周期日志
  已出现新增 driver_rx、wifi_done、driver_pbuf。21:17 无 AP 客户端，STA
  RX/TX 完成计数有效，refs=11/frees=11/held=0，内部空闲 185115 bytes。
  初始 UART 字节含乱码；使用后续完整日志行判断，不据此认定 crash。

尚需 Android 正常使用与卡顿现场，验证 AP 侧新计数有业务增量，再定位
丢失进展发生在哪一侧。不能把此次诊断烧录称为断流修复或吞吐提升。
12d 回退快照仍保留，未替换全局 IDF 或追加自动重连/强制重传机制。

## 21:20 停滞现场与 12f 统计修正

用户报告21:20彻底卡住，随后保持连接打开`http://1.1.1.1/`成功。
21:20:19日志active61/64，3条s2c积压11328bytes；flow263客户端
snd_wnd=0、unsent16416bytes、persist持续增加，上游发送队列空，接收窗口
因转发背压降为0。21:20:24前AP/STA RX都出现约9秒安静窗口；随后仍有
少量RX和TX完成增量，并非所有入口一直停止。新统计driver_pbuf的采样
held=0、refs/frees相等，内部空闲约155–166KiB，clone/parse/mapping拒绝0。
AP曾到64路，pressure_closed累计63；这说明确有容量压力，但在卡住窗口
active下降后停滞连接仍存在，不能只靠扩大连接数解释或解决。

约21:21:48—49两个积压连接出现client_exception，随后21:21:50—21:22
业务重新增长，AP关联保持不变。WSL对ESP32 STA 192.168.1.103的5次ping
全部成功（1.8–4.2ms），对Xray主机的3次ping也成功；探测发生在21:21之后，
不证明21:20最初瞬间无线正常。client零窗口与应用不读取相符，但仍不能
确定是APP本身、网络条件、既有连接行为或其他因素所致。

DNS在21:20:50有1个回复RTT53ms，21:21:50的12个回复平均10ms；不能据此
保证每一个域名或每一时刻DNS正常。UDP443累计66次发出、0回复是单独
待查项，不足以确定B站首页失败的原因。ESP目标errno113是ECONNABORTED，
不是Fedora的EHOSTUNREACH；对照了本机Xtensa工具链sys/errno.h。

发现诊断本身的竞态：`net_diagnostics_log()`先取now，回调随后更新last，
无符号now-last会误报约4294964ms。本轮12f仅将每个age计算调整为先读取
last、再采样now，保留无事件的UINT32_MAX标记和真实32位时钟回绕语义。
更改`main/net_diagnostics.c`和`tests/test_net_diagnostics.py`，测试在读取时钟
期间注入一次完成回调，以覆盖旧统计竞态。该修正不改变转发路径或断流
行为；板上继续12e，保留现场，12f未烧录。
