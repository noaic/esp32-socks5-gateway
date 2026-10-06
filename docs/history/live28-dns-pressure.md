# live28：DNS 压力回收候选（已烧录，板上应用验收中）

## 问题与证据

perf27普通视频及首页无明显卡顿。用户观察多个直播间进入后偶有缓冲停顿、稍后恢复，稳定播放后暂时正常；“启动请求/带宽突发”是待验证假设。TCP零窗口也出现在其他转发正常的时段，不能单独作为直播根因。

monitor-perf27-tx32-live.log中05:02:47—05:03:17，UDP flow=32/32，DNS in/sent/reply为247/189/145，FLOW_FULL=58、NON_DNS_LIMIT=2860，本地输入队列丢弃0。主机生产事件循环复现：28条普通UDP流＋4条未回复DNS占位，会拒绝下一条新DNS。详见performance27.md及test-live28-dns-capacity.log。

普通流最多28只保证最少4个DNS位置，不保证它们不会被无回复事务占满。旧实现仅立即回收完整收到回复的DNS，未回复状态随UDP普通60秒空闲TTL保留；重试也可能刷新该TTL。

## 修改文件与规则

- main/proxy_config.h：增加压力期限10秒，不改32总上限或普通空闲TTL。
- main/dns_flow.h、main/dns_flow.c：判断所有可追踪pending ID是否耗尽首发期限；空pending、未知/溢出报文、不合理时钟或期限均不符合超时条件。
- main/udp_relay.c：查找空槽与已完成DNS后，如果仍满，且新增流为DNS，允许回收READY、无待发数据、所有pending ID均已超时的旧DNS流。关闭旧control/UDP socket、清空旧状态，再建立新association。既有相同tuple仍复用，不无条件关闭或延长客户端事务。普通UDP不能使用此压力回收，正在工作的普通UDP不会被抢占。
- main/udp_relay.h、main/udp_gateway.c：末尾新增dns_pressure_reclaimed计数，原枚举编号保持不变，区别于正常已完成DNS回收和flow full。
- tests/dns_flow_harness.c、tests/test_udp_relay.py：增加超时边界、重试、复用端口多ID中含新查询、未知状态保护及满表集成回归；模拟peer允许DNS独立延迟，其余旧测试默认行为不变。
- README.md及本文：记录板上live28状态及perf27回退位置。

保留TX32、Core1、IRAM、RX copy和全部窗口/队列/任务配置；不引入任务、动态flow分配、DNS解析/缓存或上游配置修改。阈值是网关在满表时的资源管理策略，不是DNS协议或Xray要求。

## 验证

test-live28-udp-relay.log：15项实际生产事件循环测试通过。新增用例先建立28普通＋4延迟DNS；首发1秒内（测试用缩短期限）保护原查询，新DNS暂不入场；超过期限后新DNS建立第33个累计association并收到正确ID回复，旧普通流仍能收发且不重建association。旧的严格动态relay、共享relay、分片握手、重连、DNS隔离、generation reset、EAGAIN与本地输出重试等回归仍通过。

test-live28-dns-flow.log：事务计时边界、相同ID重试维持首发期限、混合旧/新pending ID、已完成/空pending以及未知报文保护通过。build-live28-dns-pressure.log：ESP-IDF6.1构建成功，无warning/error，硬件配置仍esp32s3/16MB Flash/Octal PSRAM、staticTX32、TCPIP Core1。

可重复命令：

```sh
PATH=/usr/bin:/bin /usr/bin/python3 tests/test_udp_relay.py -v
gcc -std=c11 -Wall -Wextra -Werror -I main tests/dns_flow_harness.c main/dns_flow.c -o tests/.out/dns_flow_live28
./tests/.out/dns_flow_live28
source /home/user/.espressif/tools/activate_idf_v6.1.sh
idf.py build
```

## 状态、限制与板上验收

用户退出直播并要求继续后，2026-10-04 05:16烧录成功，flash-live28-dns-pressure.log记录hash校验成功；未修改上游或全局IDF。monitor-live28-dns-pressure-live.log持续采集，确认新dns_pressure_reclaimed字段存在、上游STA已关联（channel4、RSSI约−32dBm），尚无AP客户端时内部空闲114603bytes、最大连续块69632bytes、PSRAM空闲6990904bytes、RX held0/peak1、发送错误0、PCB分配失败0。未捕获完整启动前段，不将这些快照当作每项硬件启动日志的完整验收。

当前候选有构建、主机回归和板上启动证据，直播应用验收进行中，不能宣称直播已修复。请求Android重连AP并播放一个直播间后切换两个，记录新直播间进入与稳定播放差异，核对dns_pressure_reclaimed、FLOW_FULL新增数、DNS发送/回复及控制关联状态；再检查实际heap与普通UDP。host测速服务标签已同步live28-dns-pressure，当前不要求测速。

未满表时保留普通60秒TTL；压力下可能丢弃超过10秒才到达的旧DNS回复，客户端须自行重试。重复同ID没有新首发预算；同关联中的新ID仍保护整个关联，未知/溢出/未发送/握手中状态也受保护，因此此策略不能保证任何DNS突发都成功，更不能修复上游不回复。新增流在超时前仍可能被拒绝，需要客户端重试。未调整直播协议、UDP443策略或TCP零窗口逻辑，未新增外部探测。

回退可使用build-snapshots/perf27-tx32保存的固件；项目中保留本候选源码和测试记录，不手改全局IDF。

## 第一轮直播验收：DNS回收有效，进入期停顿仍存在

用户尝试3个直播间，均在进入后5秒内出现加载暂停，约6秒后恢复，稳定播放阶段正常。05:17—05:21采样中dns_pressure_reclaimed累计3、FLOW_FULL始终0，新增DNS已能入场；UDP443始终0，本地UDP输入队列/发送/输出丢弃0，TCP活动最多31、PCB分配失败0。成功匹配的DNS回复RTT最大170ms、DNS发送等待最大128302us，仍有未匹配/未回复请求，不能将这些已回复样本的延迟用于证明所有DNS都正常。

Core1 idle最小采样0.3%，内部空闲最低87779bytes；存在进入期负载峰值。05:20:55的flow99目标175.4.62.20:443有客户端零窗口、unsent16960、应用s2c4416，AP客户端与STA路由ping各3/3成功；05:20:56另有上游connection reset（116.207.163.3:443）。未获得房间/流对应关系，不能认定这些连接就是直播媒体；10秒日志分辨率也不足以对齐6秒停顿。板上已确认满表DNS压力回收运行且本轮不再发生FLOW_FULL，尚未解决直播进入期停顿。

哔哩哔哩技术团队的历史文章《B站直播的自研P2P实践》（2022-11-06）介绍HLS服务器初始下载、WebRTC/P2P交换及服务器补完流程：https://www.bilibili.com/opus/725332930364178435 。当前官方第三方目录列有安卓/iOS腾讯P2P组件：https://www.bilibili.com/blackboard/activity-mggnAmXZHi.html 。历史文章主要描述浏览器P2P，不是用户当前Android版本的协议证明；SDK目录也不能证明本次直播启动了P2P。

本机代码udp_relay.c每tuple独立association，并要求SOCKS回复源IP/port等于该flow目标；这是普通点对点UDP转发，未实现P2P/NAT打洞的特殊语义，符合V1范围。普通UDP多发少回与进入期恢复现象使“P2P尝试后服务器补拉”成为待验证假设，不能靠扩大TX或禁用UDP直接确认。未修改上游/VPS、UDP过滤或QUIC策略。

下一步请求同手机连接UPSTREAM_WIFI、进入相同直播间对照。直连与代理的路由/DNS/CDN选择可能不同：若直连也停顿，支持App/服务侧进入流程因素；若仅网关停顿，只能缩小到整个代理路径，不能单独证明ESP32硬件根因。继续保留live28采集，暂不叠加参数。
