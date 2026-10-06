# perf27：高吞吐下静态 TX32 对照

## 修改与依据

仅sdkconfig.defaults及本机sdkconfig：CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM由16到32。cache TX32、static RX16/dynamic RX48、TCPIP Core1/Wi-Fi Core0/proxy Core1、IRAM/RX copy/存储/窗口/优先级均保持perf26。ESP32_WIFI旧名字为本机rename生成别名，修改时删除其旧16定义后随现代选项再生成；独立WIFI_RMT配置未改，不把远程Wi-Fi数值当作本板驱动数值。

本机v6.1 esp_wifi/Kconfig明确static TX范围1—64，每buffer约1.6KB；上层交付过快可能耗尽static TX，cache queue用于没有可用static TX时暂存。实际额外内部RAM通常约27KB（需板上确认）。增加容量不等于一定增速，不能掩盖Core1计算饱和。

此前perf18在约10Mbps低吞吐时TX32无确认收益且成本约27.1KiB，已撤回。本次perf26四路已达20.41/17.59Mbps，driver TX mem计数AP约2%、STA约1.4%，有新的高负载压力依据；是重新对照，不推翻旧低负载结论或将容量加倍等同速度加倍。

## 新会话与当前基线

04:38确认真实/dev/ttyACM0仍存在（root:dialout，nine为dialout成员），WSL IP仍192.168.1.10。之前长时采集/host服务已停止，本轮重新打开UART（不主动复位）及主机服务。板子仍perf26，启动后没有AP客户端时内部heap采样142443bytes、最大块98304bytes；当前STA/host SOCKS5路径健康，通过上游GET /bench成功。

距上一轮已过几小时，先采集新四路基线，避免用旧时段无线/后台条件直接判断候选。baseline标签perf26-tcpip-core1，结果bench-perf27-baseline-results.jsonl，UART monitor-perf27-baseline-live.log，host /tmp/esp32-perf27-baseline-peer.log。候选已经编译但基线完成前不烧录。

## 构建与对照流程

build-perf27-tx32.log构建成功，无编译器warning/error；生成配置TX32、TCPIP=1确认。build-snapshots/perf27-tx32/含固件、config/main/manifest，含本机Wi-Fi配置勿分享。已有官方整数参数不添加镜像单测，本轮验证配置/编译/实际heap、吞吐、CPU、RX held及错误增量。

Android同页http://192.168.1.10:18080/bench确认标签，先当前perf26四路3轮。完成后才部署候选，更新host label为perf27-tx32、结果bench-perf27-results.jsonl、UART monitor-perf27-tx32-live.log，再同条件四路3轮。只拿同阶段driver调用/error差分计算负载期间错误率，不能用累计数字直接混比不同固件开机时间。记录最大块及内部heap，必要时再看实际应用。

## 已采集但受后台流量影响的基线

2026-10-04 04:42:57 完成的四路 run `musuww29-6jrf70rmj4d`：下载/上传分别为16.14/14.57、15.99/13.89、12.10/12.83 Mbps，中位数15.99/13.89。之后单路 run `musv3bsv-v3zr80pz69` 中位数9.08/8.03 Mbps。两组均自动保存于baseline JSONL。

此时日志持续active=64/64，TCP PCB peak=256/256，metadata曾447/512，并有大量非测速目标活动和capacity idle close。04:46:32 CPU1 idle仅12.1%，proxy_tcp占60.0%、tcpip占24.0%；RX held=0、peak=1。PCB allocation failures是分配器观测值（可能随后回收重试），不能直接等同失败连接数。此轮并非空闲条件下的纯测速，不能与几小时前20.41/17.59直接比较，也不能拿它对比刚重启清空连接的候选便宣称TX32增速。候选暂未烧录，先要求关闭其他联网应用与标签，等待连接回落后重测。

用户关闭其他联网应用和浏览器标签后，04:47:42活动连接降到15；04:48:05 Core1 idle为92.4%、PCB live139、allocation failures保持227，metadata269/512逐步回落。这支持后台负载影响本轮测试，尚不能归因于具体应用。板子保持perf26未复位，补测受控四路基线后再部署候选。

## 受控基线

关闭其他应用后，2026-10-04 04:49:53完成四路run `musv6jkf-sb3otc6ia5b`。三轮下载/上传19.84/17.44、19.94/17.84、20.35/17.81 Mbps，中位数19.94/17.81，接近此前perf26结果。随后部署TX32候选，保持其他应用关闭；保留污染基线原始数据，不用它计算候选收益。

## TX32 第一组结果（尚待重复及应用验证）

烧录成功，flash-perf27-tx32.log记录写入校验。2026-10-04 04:53:18完成run `musvb2qn-tkv6h5078ca`，三轮下载/上传21.63/19.08、21.67/19.24、21.30/19.46 Mbps，中位数21.63/19.24，较受控基线增加8.5%/8.0%。结果与分析存于build-snapshots/perf27-tx32/results.json。

发送错误按测试开始前最近一次及结束后最近一次UART采样做差（不是精确请求边界）：基线AP 5389/126811=4.25%、STA 4700/124582=3.77%；候选AP 85/124025=0.069%、STA 78/120515=0.065%。这表示驱动返回内存不足的调用比例，并非最终TCP数据丢失率。基线采样区间04:48:26—04:49:56，候选04:51:50—04:53:22。

两组活动代理连接均最高17，Core1 idle中位数分别2.5%/2.3%，计算瓶颈仍在。测试区间内部空闲最小值132695/100831 bytes、最大连续块最小值73728/57344 bytes；不能将两个负载采样差值全算成静态TX成本，需参考相同无客户端状态。候选PCB live最高104、开机至本组后allocation failures=0；基线此前存在TIME_WAIT等历史状态，不能把重启后的PCB减少归因于TX32。候选RX held=0、peak=1，TCP重传观测计数为0，rewrite解析/映射/shape丢弃为0；短时无watchdog/crash记录。

## 第二组与保留决定

同固件、同连接完成run `musveac8-wccezq7zf3c`（04:54:29—04:55:45），三轮22.49/19.80、22.54/19.87、22.30/19.89 Mbps，中位数22.49/19.87。两组六轮合并中位数21.99/19.63，较本轮受控基线增加10.3%/10.2%。第二组发送采样区间04:54:24—04:55:55，AP mem增量74/123333=0.060%、STA 96/120211=0.080%，改善重复。

第二组内部空闲最低106087bytes、最大连续块最低59392bytes，Core1 idle中位数2.05%，活动会话最高15。两组后PCB allocation failures仍0、RX held0/peak1、TCP重传观测0、DNS28/28/28且UDP队列及本地输出错误0，未记录watchdog/crash。无AP客户端时perf26内部空闲142443bytes、perf27为114599bytes，差27844bytes（约27.2KiB）；不同启动采样存在小量误差。不同流量/连接历史下的heap变化不能单凭两轮排除泄漏。

保留TX32配置及板上perf27，README同步当前状态。下一步仅请求正常B站/网页应用验证，不增加参数；单路成绩沿用perf26历史标注，未把四路结果当单路或公网速度。无线时段、重启清理TIME_WAIT以及没有回切TX16重复是本次比较的限制，较稳妥结论是同条件两组吞吐与发送错误改善均可重复，长期稳定性仍待观察。

## 风险与恢复

## 实际应用：普通视频通过，直播问题未解决

用户反馈首页及普通视频没有断流或明显卡顿，但切换直播出现缓冲后卡住。现有日志尚未与具体直播间及用户时间对齐，不能把某个IP标记为直播服务器，或把吞吐提升称为直播修复。

05:01:13—05:01:44连续观测flow333，目标111.172.208.125:443、上游本地端口50332。客户端snd_wnd=0、unsent12064、unacked0，应用s2c剩1408bytes，persist探测递增；映射仍open，rx_packets1374→1375，说明至少有新的入站报文。上游rcv_wnd降至0是下游背压后的本地停止读取，不能据此称Xray不接收。该时段AP-client和STA-router探测均3/3成功、延迟0—6ms，其他流量字节继续增长、RX held0/peak1；不符合此前全部无线RX缓冲耗尽故障。

05:00前后的UDP443累计42发送/0回复，DNS存在请求与回复数量差且UDP本地队列/发送/输出错误为0。统计差可能含未完成、重复或上游无响应请求，不足以确定DNS故障或直播依赖QUIC。PCB历史峰值256，分配失败32且后续未增长；不能排除早期容量压力，但卡住快照活动会话约26—39，未达64。保留TX32，不更改Xray/VPS或强制复位；先获取直播间、发生时间及退出重进表现，进一步定位。

用户随后澄清多个随机直播间均会播放一段时间后缓冲停顿、等待后恢复。05:01:13—05:02:14同一flow333的客户端零窗口持续至少64秒，但其他TCP转发字节继续增加；不能确认它属于直播，也不能将手机停止读取直接归因为App缺陷。后续用户无法调整清晰度，保持一个直播间播放，暂时没有卡顿。固件没有改变。

### 已确认的独立 DNS 容量拒绝

05:02:47—05:03:17 UDP flows=32/32，DNS统计由197/189/145到247/189/145（in/sent/reply），UDP_FLOW_FULL累计58，NON_DNS_LIMIT累计2860；网关输入队列丢弃仍0。此处新DNS确实没有发往上游，不能再解释为仅上游回复迟到。ordinary限28只保留了最少4个DNS位置，但4个未完成DNS也会占满它们；只有收到完整匹配回复且无待处理ID的DNS槽位才允许提前回收，其他保持普通60秒空闲TTL。这种保留并不保证连续新增DNS可入场。

使用tests/test_udp_relay.py的生产UDP事件循环harness和本机模拟peer复现：先建立28条正常普通UDP flow，再建立4条延迟回复DNS flow，下一条独立DNS query没有打开association、UDP_FLOW_FULL递增。结果见test-live28-dns-capacity.log。测试仅用回环套接字，无外网/ESP32/无线依赖，不修改生产代码。现有容量测试只覆盖成功回复后的DNS回收，未覆盖未回复占位。

下一步修复应独立验证DNS在普通UDP占位及上游DNS不回复时的准入/回收行为，保持32总上限、严格association及事务隔离；不能简单扩容、抢占正在工作的流或未经超时便回收未完成DNS。直播因果仍待同步现场，当前未部署DNS行为改动，也没有宣称直播已修复。

核对最新Xray DNS outbound源码：https://github.com/XTLS/Xray-core/blob/main/proxy/dns/dns.go 。A/AAAA走handleIPQuery、非IP类型由applyRules决定处理；不能将所有请求无回复直接归因于UseIPv4或UDP出口限制。未修改上游配置或发起QUIC探测。

32比16多占内部RAM，PSRAM剩余不能抵消内部余量；当前负载最低内部空闲约98KiB，仍需实际多会话应用验证。若收益不足、错误不减、并发内存或应用退步，恢复现代CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM=16同步defaults及本机sdkconfig重新构建/烧录，或用已保留perf26快照；保留Core1等已获确认收益的改动。短时正确性及轻微时段波动不等同长期无泄漏/稳定。

候选app 807104bytes，ELF SHA256 f751f9861d61864c1069812b6a9e7924627b4c4879c16d2d6928d676ec01a501。
