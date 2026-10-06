# 两客户端候选：multi40

2026-10-04。在diag37收尾稳定版上，最大AP接入从1改为2，不扩64TCP、32UDP、512映射总容量；17280发送缓冲、23040接收窗口、8KiB双向载荷队列、Wi-Fi缓冲、AMPDU、HT40和核心绑定保持不变。新增pipeline采样和AP本地HTTP诊断仍关闭。

## 可行性与边界

官方ESP-IDF v6.1的 [wifi_ap_config_t.max_connection](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/network/esp_wifi.html#_CPPv4N16wifi_ap_config_t14max_connectionE) 支持配置AP接入名额。本机对应esp_wifi_types_generic.h/API已核对。单射频AP+STA仍共享空口；两台不意味着总吞吐翻倍，低速/弱信号终端可能拖低整体效率。此前单客户端多流Core1已接近满载，双客户端预期收益是同时使用和连接隔离，而不是提高带宽上限。

当前版本要求AP客户端使用DHCP自动获取IPv4，DNS仍下发1.1.1.1。不新增NAT、DNS服务器、Web UI或IPv6。IPv4来源不是安全认证；不承诺抵抗恶意源IP伪造。客户端间本地通信也没有新增隔离策略。

## 生命周期与准入

- ap_clients.c/.h维护两个静态MAC/IP/token记录，FreeRTOS短临界区串行化。连接时生成新token；IP改变也生成新token；相同IP的DHCP续租保留token。另一客户端不能被同IP分配静默覆盖。AP停止使所有客户端失效；STA断开仍沿用全局网络epoch。
- 使用本机v6.1的IP_EVENT_ASSIGNED_IP_TO_CLIENT和ip_event_assigned_ip_to_client_t，不用弃用的AP_STAIPASSIGNED。检查了esp_netif_lwip.c的MAC/IP事件字段及dhcpserver.c：成功发送DHCP ACK后发布回调，续租也有ACK。事件到达前的透明业务包可能暂时丢弃，依赖客户端正常重试。
- 客户端断开只使该token失效，不调用全局TCP/UDP reset。worker在下一轮及select之后检查token，自己关闭对应socket；UDP输入队列、回复投递callback也检查token。已进入驱动/空口的在途帧不在此承诺范围内。
- TCP独立映射存token。相同源IP/端口/目标在新会话可创建新映射，旧映射保持lwIP关闭保护期，不提前释放旧端口标识；reverse output拒绝失效token。
- 两台关联时，每台新TCP准入上限48、UDP上限24，其中普通UDP最多22。总上限仍64/32，DNS全局预留4路。只有一台时可借用全部现有容量；第二台加入不强杀既有业务，因此第一台若已占满，第二台可能等待正常回收。准入限制不是严格平均分带宽，也没有每客户端队列或DNS速率保证。
- TCP每循环轮换先服务的session；UDP保留既有cursor轮换和DNS优先。驱动调度、每条TCP的拥塞控制仍各自负责；不实现全局QoS。

## 固定资源变化

根据新旧目标ELF的GDB sizeof：

| 对象 | 原版 bytes | 两客户端 bytes | 增量 |
| --- | ---: | ---: | ---: |
| 两客户端表 | 0 | 144 | 144，另有锁/序号等小对象 |
| TCP session每条 | 128 | 128 | 0，使用原padding |
| TCP映射表512条 | 24840 | 28936 | 4096 PSRAM |
| UDP flow每条 | 6128 | 6136 | 8，32条共256 PSRAM |
| UDP datagram | 1496 | 1504 | 8，输入队列/暂存/投递共33份增加264 PSRAM |

没有新增task/socket池/载荷缓冲，总固定PSRAM约增加4616 bytes。两台实际使用时仍会增加动态TCP pbuf/segment、PCB、ARP/Wi-Fi站点上下文；这些不能只用结构大小推算。每个token检查、流量计数及轮换也有CPU成本，需用同一测速端点评估单客户端回归与双客户端总吞吐。

## 指标与测试

每10秒打印逐MAC RSSI，逐token IPv4、活动TCP/UDP、准入拒绝数、累计TCP上传/下载payload bytes、UDP发送/成功本地输出包数。映射表按token拆分使用量，旧客户端映射单独记录；补充内部堆启动以来最低空闲。继续采集CPU0/1、任务栈、当前internal/最大块/PSRAM、PCB live/peak/失败、TX memory/buffer、RX held、TCP RTO/fast、DNS RTT/等待/队列丢弃。计数是当前会话累计值，重连或IP改变清零；需按时间差计算速率。

构建build-multi40.log成功，无warning/error；app810912 bytes，SHA256 0f78aeac759ed9ec47dab3d53fda50479dac3767d4e16850b72b5652f662af18。flash-multi40.log烧录/hash校验通过。

主机回归：TCP16项、UDP16项全部通过，新增客户端生命周期/准入/映射隔离C harness通过；同步测速barrier两项通过。另补测同源端口的TCP/UDP选择性断开：另一台持续可用，断开重连及旧UDP回包隔离通过。原flow table满容量、hash unlink/reuse与关闭保护harness通过。主机测试不能代替真实Wi-Fi双客户端验收。

修改文件：main/ap_clients.c/.h、proxy_config.h、app_main.c、flow_table.c/.h、tcp_poc.c、socks_relay.c/.h、udp_relay.c/.h、udp_gateway.c、udp_delivery.c（未启用重试，但保留datagram token）、CMakeLists.txt；测速tools/test_stream_server.py、throughput_bench.html；相关tests和本说明。未patch全局IDF。

原测速地址仍为http://192.168.1.10:18080/bench，标签multi40-two-clients。两台分别选择A、B；先各自单路测试（另一台空闲），再两台各点“双终端同步测速”。开发主机barrier对齐每轮下载/上传，结果记录client_id、pair_id、run_id、各轮耗时和内容校验。UDP/SOCKS源端隐藏了HTTP原始IP，因此A/B是用户标识，不把开发主机看到的peer当成客户端身份。同步精度受HTTP延迟/浏览器调度影响，不能作精密物理链路测量；计算合计吞吐应按共同阶段时长，不能简单相加不同时间窗口的中位数。

结果自动写bench-multi40-results.jsonl，串口monitor-multi40-live.log。下一步先验收两台同时DHCP/上网，再采集单台与同步测速，最后其中一台主动断开时另一台播放/下载是否连续，以及重新加入能否恢复。双客户端性能和稳定性目前待实机评估，不宣称完成验收。

回退：保留diag37-stable完整bin/ELF/sdkconfig快照。若需回到单客户端先恢复整套稳定快照；仅将max_connection改回1仍会保留本候选的token/准入开销，并不是原版性能回退。

## 首轮接入验收

用户确认两台同时连接并上网。串口记录da:e6:3f:e4:9c:b8 / 192.168.4.2和36:9f:e2:8f:6e:d1 / 192.168.4.3，两个token均有TCP/UDP转发流量。第二台UDP曾达到24且出现52次准入拒绝；TCP PCB峰值256/256（含关闭/TIME_WAIT），未观察到分配失败。这些是非受控普通App/网页负载，不作为测速成绩，也不证明长稳通过。原始日志monitor-multi40-initial.log和initial-analysis.json保留。

最后复查调整UDP满表回收顺序：不同owner的已完成DNS流只在新请求通过客户端准入后回收；同owner替换先归还旧额度。未改变容量和无线参数，UDP16项回归再次通过。最终固件已构建，烧录校验记录见flash-multi40.log。

## 两台依次单路测速

两台同时关联，依次单路测速、另一台空闲。2026-10-04 15:28:00–15:28:32，A三轮下载/上传12.31/11.57、13.82/12.36、12.92/14.26 Mbps，中位12.92/12.36；15:30:35–15:31:10，B重测12.55/11.08、12.42/11.53、12.83/10.39，中位12.55/11.08。两组phase=complete、visibility=visible，下载全字节及上传SHA校验通过。

B此前muti278o-qudepflnkh8仅上报两轮，其中慢上传耗时48.5秒，visibility=hidden，用户确认看错完成状态并手动结束。该组不能计算三轮中位数，排除性能对比；不把后台状态与慢速的相关性当作固件故障归因，也不认为手动结束的测试是网关断流。

A/B完整CPU窗口Core1 idle中位28.65%/25.9%，proxy_tcp36.55%/41.1%，tcpip31.95%/31.3%。内部空闲最低104003/105207、最大块63488/61440、PSRAM最低6925148/6930268 bytes。10秒采样PCB live最大65/85，不等于精确瞬时峰值或启动以来peak。前后相邻driver采样AP memory返回均0（32219/31781调用），STA均0（30696/30694调用），未见准入拒绝。全局TCP重传计数包含其他业务和已结束测试，不能把B重测末尾累计上游RTO1/fast3全部归给重测。

本组单路大致处于历史范围，但不与非同期、不同背景/设备的旧结果作严格因果对照，不能排除新增token检查/统计的开销。下一步双终端同步单路测速，对齐下载与上传阶段后评估总吞吐、公平性和两台满载CPU/内存。原始结果/UART和sequential-analysis.json已存快照，包含被排除的部分报告。

## 双终端同步测速

2026-10-04 15:34:39–15:35:39，pair_id ec4fd8f1771b4633a0615b893fe9a7e2，A/B各三轮、每台单路，phase=complete、visibility=visible。两边下载全字节、上传SHA校验通过。

| 终端 | 三轮下载 Mbps | 三轮上传 Mbps | 下载/上传中位数 |
| --- | --- | --- | --- |
| A | 8.99、8.22、8.72 | 10.38、8.32、9.38 | 8.72 / 9.38 |
| B | 8.33、8.11、8.37 | 6.04、7.56、7.59 | 8.33 / 7.56 |

每轮共同阶段两台各8MiB，合计16MiB除以较慢终端的阶段耗时，估计下载16.67、16.21、16.75 Mbps，中位16.67；上传12.07、15.13、15.19，中位15.13。不是相加两个设备的中位数；也不是瞬时重叠区间峰值，包含较快设备完成后等待较慢设备的尾部。浏览器/HTTP启动偏差未精确测量，估值应保留此限制。

完整CPU窗口Core0 idle中位55.2%，Core1 idle18.5%，proxy_tcp38.0%、tcpip42.0%。这些窗口包含同步barrier、方向切换和只剩较慢设备的尾部，不代表两边持续满载时均有18.5%余量。内部空闲最低106187 bytes、最大块63488、PSRAM最低6910624；10秒采样PCB live最大63。相邻driver窗口AP64207次、STA61345次调用，memory返回均0；全局客户端RTO1/fast0、上游RTO1/fast3在前后相邻采样中未增长。逐客户端准入拒绝均0，没有STA/AP断开或重启记录。

本轮支持两个普通终端同时传输；下载接近，上传A更快，不承诺严格均分。单客户端四路历史总吞吐约20Mbps，两客户端各单路本次约16.7Mbps，流数、终端、空口竞争和测试窗口均不同，不能直接归因于两客户端代码退步，也不应宣称带宽上限提高。仍需实机选择性断开/重连接续验收；普通App的UDP突发、接近PCB上限的场景和长时间稳定性尚未充分覆盖。原始数据、UART、paired-analysis.json保存在快照。

## 实机断开隔离验收与收尾

2026-10-04 15:38:26–15:38:56，A单路mutiefcl-gntmmbn7yzr三轮下载/上传14.39/14.02、15.55/14.10、11.24/13.71 Mbps，中位14.39/14.02；phase=complete、visibility=visible，内容校验通过。此组夹杂B断开重连，不作为纯稳态速度对照。用户确认A完成、B重连能正常上网。

B于15:38:34.126断开；15:38:34.137–.175，worker清理flow174、175、177、178（operation=network_or_select，errno=0，对应旧B会话）。B于15:38:48.293重新关联，15:38:48.469 DHCP仍分配192.168.4.3，token从5变7。A token全程为2，持续转发且测试完整结束；B新token有TCP/UDP流量。没有AP停止、STA断开或重启记录，说明这次没有全局清理其他客户端。

客户端侧全局RTO计数从1增长到测试末尾附近8，上游仍RTO1/fast3，driver memory仍0。该计数包含其他业务及关闭状态；没有逐流证据，不能全归因于主动退出的B，也不能宣称断开测试零重传。它没有阻止A校验完成。细节已存isolation-analysis.json及原始UART。

结论：最大接入2、两台DHCP及互联网访问、顺序单路及同步传输、选择性断开重连接续均完成基础验收。保留当前multi40，未再扩TCP/UDP/无线缓冲或启用pipeline采样。两台共享带宽，不承诺每台固定速率；上传存在终端差异。固定资源增量较小，真正风险是普通App突发时的UDP准入拒绝和PCB接近256（首轮非受控浏览已观察到），长时间稳定性和稀有网络中断仍未充分覆盖，不把本次有限验收视为全部容量/稳定性证明。

本轮收尾文件：README.md、docs/multi40.md、bench-multi40-results.jsonl、monitor-multi40-live.log和build-snapshots/multi40-two-clients内原始结果、分析、源码、sdkconfig、ELF/bin、构建/烧录/回归日志。当前固件未更改，不需要再烧录。WSL测速入口仍使用原18080端口，便于后续复测；基本使用不需要测试页面参与。
