# 稳定基线后的吞吐调查与分轮测试

## 基线与公开资料

用户报告rx17：上传7.62Mbps、下载7.99Mbps，近5分钟未卡死。
固件、ELF、配置及main源码保存于build-snapshots/rx17-l2-copy/，
speedtest-baseline.json记录本次结果（仅单次，服务器未记录）。
rx17的L2_TO_L3_COPY修复必须保留；不能为了零拷贝吞吐回到RX池耗尽。

已读本机6.1的esp_wifi/Kconfig、lwip/Kconfig、wlanif.c、lwipopts.h、
官方iperf配置及项目TCP/UDP数据路径。公开官方资料：

- https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html
- https://github.com/espressif/esp-idf/blob/master/examples/wifi/iperf/sdkconfig.defaults.esp32s3

官方单段单流TCP结果不等于AP+STA SOCKS网关。S3仅一套2.4GHz
无线收发，同信道收一次、发一次，还有ACK、争用和软件开销。
不能把屏蔽箱数字或PHY速率当成本项目承诺；也不能把某个单段
吞吐简单除以二就声称是此板的硬上限。现有约8Mbps有实际约束，
是否能提高需要受控测试，不能凭配置表保证翻倍。

WSL经同一192.168.1.2:7897 SOCKS5、绕过ESP32下载Cloudflare
普通HTTP 8MiB：HTTP200、2.088481s、4016607B/s（约32.13Mbps）。
该时刻上游有超过8Mbps的能力，但服务器/时间/客户端路径与
Android SpeedTest不同，不能把差值全部算成ESP32开销。停止QUIC
探测的决定保持，未更改Windows/VPS或Vision配置。

## 代码与配置审查结果

已启用CPU240MHz、PERF编译、Wi-Fi IRAM、32KiB指令/64KiB数据
cache、AMPDU RX/TX、WIFI_PS_NONE，双端TCP_NODELAY。TCPIP/Wi-Fi
在CPU0，TCP/UDP固定worker在CPU1。MSS1440，snd17280、wnd23040，
TCP接收mbox18符合本机Kconfig建议wnd/MSS+2；无需随意调大mbox。

TCP事件循环的100ms select超时是无就绪事件时的等待上限，不是
每个包固定等100ms。新收数据已有立即send快路径。方向缓存8192B、
每轮每方向一次send保持公平，现阶段未测到CPU占用，不能宣称
每连接task或无界drain更快。改写已做RFC1624增量checksum；输入
仍校验原TCP checksum，输出只读必要头部，不再扫完整payload。

后续候选按证据/风险排列：

1. TX静态缓冲16→32：AP已有ESP_ERR_NO_MEM发送事件，官方Octal
   PSRAM配置使用32；额外16*约1.6KiB，预计约25—26KiB内部RAM。
   可能改善突发资源压力，但其出现频率不高，未证明是8Mbps主因。
2. L2 copy后的AP输入目前再次pbuf_clone。可研究只对独占、连续
   PBUF_RAM原地改写，其他情况保留clone；需要验证引用/错误释放。
   输出clone必须保留，因为lwIP重传/ARP引用不能被改写污染。
3. lwIP IRAM选项当前关闭。本机Kconfig普通项约10KiB、额外TCP项
   约17KiB IRAM，普通项双核收益可能有限。需结合CPU/内部RAM测量，
   不直接套官方全部iperf参数。
4. send/window扩大、relay缓存布局/批量收发、核心分配：先量化
   CPU、就绪处理/窗口与发送压力，再分别试。窗口对内存按连接数
   叠加，64*2连接不可照抄单流85KiB窗口。
5. PCB峰值256和关闭状态堆积影响高频开连接，不等于稳态大流
   带宽不足；先统计状态，再考虑关闭回收策略。保留正确FIN/TIME_WAIT。
6. 无线干扰/Android省电与共用空口：先测局域网；此前HT20、关闭
   AMPDU RX、AP仅b/g均退化，不重复盲调或宣称提升天线能解决代码问题。

## 临时局域网测试（不经过互联网/VPS）

Windows Xray可通过现有私网规则访问WSL192.168.1.10:18080。
已从WSL经SOCKS5验证health、测速页、8MiB下载和上传：下载
0.083865s、上传0.064826s，内容SHA256
7d212b9c884f5c77896de960ae17cc341cda43b14d6a971f34ca29ebd4badf7f，
上传matches_8m=true。这是绕过ESP的局域网回环验证，不是网关速度。

路径为Android→ESP AP→透明TCP→SOCKS5 Windows→WSL测试服务。
仍包含Windows/Xray与WSL虚拟网卡开销，但能排除远端公网出口波动。
无USB无线网卡，无Windows portproxy/防火墙/系统路由改动。

修改tools/test_stream_server.py：保留原health/8m/upload/reset测试接口，
增加/bench与--bind；新增tools/throughput_bench.html提供3轮各8MiB
下载/上传、中位数、120秒单次超时和内容检查。下载逐字节检查在计时
外完成，上传由服务端SHA256验证；上传计时包含服务端校验和响应。
不使用外部JS/CDN，不添加ESP32 Web UI或管理面。
Python语法、页面JS模拟3轮/6传输、实际上游SOCKS端点校验通过。
浏览器模拟不等于实际Android兼容性/时序，仍需现场测试。

服务当前仅绑定WSL eth0的192.168.1.10，地址重启可能变化。
运行在前台进程，测试后停止即可；不注册系统服务。

Android保持移动数据关闭、关闭视频/其他下载，在浏览器打开
http://192.168.1.10:18080/bench ，点击开始并保持前台，回复中位数。
每轮传输16MiB，两方向总计48MiB。不能拿单流局域网结果与公网
多连接SpeedTest直接比较；使用同页、同设备做固件A/B才有意义。

## 第一轮 perf18-tx32 的改动/验证

- sdkconfig.defaults及本机sdkconfig只将ESP_WIFI_STATIC_TX_BUFFER_NUM
  从16改32，其他窗口/聚合/核心/队列及L2 copy保持原值。
- build-perf18-tx32.log：构建成功，无warning/error。
- test-perf18-patch.log：项目本地lwIP patch校验通过，全局IDF未改。
- app801200bytes；ELF SHA256
  02182a76cf2d9792e98962ff253559c07f7d96d3efb160e5bfa6d71adaba62c0。
- 快照build-snapshots/perf18-tx32/保存候选。尚未烧录；板子仍为rx17，
  先采集同页的稳定版局域网基线，再烧录候选复测，不能混淆版本。
- 不为单个Kconfig值添加镜像测试，实际差异需上板检查内存、TX错误、
  RX held、重传、稳定性以及同页速度。

当前测试服务输出/tmp/esp32-perf18-peer.log，基线UART仍保存于
monitor-rx17-live.log。此前已获授权继续烧录测试；等待基线结果是
实验顺序要求，非再次要求用户授权。

回退硬件用rx17-l2-copy快照；源码恢复defaults及sdkconfig的TX=16
后重新构建，不能只改defaults。候选回退不会关闭接收copy修复。
快照含本机Wi-Fi配置，不分享。

## 稳定版同页实测与候选部署

Android rx17三轮下载/上传Mbps：10.16/8.90、9.72/9.10、10.02/9.13；
中位数10.02/9.10。用户仅浏览器即可完成。主机服务日志看到来源
172.29.144.1，是Windows的WSL虚拟网卡侧地址：真正连接WSL的是
Windows Xray，并非Android直接绕过NAT。

基线22:37—38期间AP发送ESP_ERR_NO_MEM累计124没有增加，TCP
重传也没有增加；所以之前视频高并发下TX资源压力不能直接当作
本次单流速度瓶颈。TX32仍作为单参数候选测试，收益可能有限。
基线窗口另存baseline-perf18-lan.log，完整日志仍在monitor-rx17-live.log。

候选已于随后烧录，flash-perf18-tx32.log hash校验通过；板子现在为
perf18-tx32，持续日志monitor-perf18-tx32-live.log。保留原固件快照
和配置。之前“尚未烧录”是基线采集前状态，以本段为最新状态。
候选同页Android实测结果尚待采集，不宣称速度提升。

## 最终结果

TX32三轮下载/上传Mbps：10.34/9.35、10.57/8.71、9.85/9.16；中位数10.34/9.16。相对rx17为+3.19%/+0.66%，与轮间波动相当；内部空闲内存减少约27.1KiB。未确认收益，恢复TX16。结果保存在对应快照results.json，后续perf19也使用TX16。
