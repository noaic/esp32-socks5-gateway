# Android 视频与首页加载排查

## 板上证据

用户报告 B 站初始视频可播放，但拖动进度失败；首页刷新需4～5秒，偶尔缺封面。
先保持第四轮带宽参数，通过 UART 对齐新复现；新增诊断版并未改善用户体验。

`monitor-android05-diag.log` 的19:09:36窗口显示：

```
flows=32/32 DNS=50/30/30
drops ... flows=20 ...
```

DNS字段依次为累计入包/发送到SOCKS UDP relay/收到对应远端UDP响应。
到19:10:17累计DNS=122/31/31、flow满丢包89、输入队列满丢包2。
已发送DNS均有回包，此时association为READY；报文过大、输入校验、socket、
发送、回包校验、TCPIP callback和AP输出失败计数均为0。
因此至少有一项明确的本机问题：已完成DNS查询也按普通UDP保持60秒，
Android使用不同UDP源端口的查询很快占满32条表，后续DNS被静默丢弃。
这能导致新域名/封面访问重试，尚不能证明所有拖动或首页问题都由它引起。

初始第四轮固件的新记录 `monitor-android05.log` 还在19:05:07出现
UDP控制TCP ECONNABORTED(ESP errno113)，19:05:40恢复。
这个已有的间歇性链路问题没有因本次DNS流回收而根治。
新记录的TCP活动采样没有到64路上限、metadata未到512，
并不支持直接用增加TCP连接数解释这次问题。

## 修复与架构

- `main/dns_flow.c/.h`：只观察UDP/53标准DNS QUERY/response头部的ID和QR/opcode。
  每flow固定跟踪最多8个待响应ID，同ID重试不重复计数；不同ID可以并行。
  匹配回复后移除ID；尚有待响应ID时不允许容量回收。
  短包/其他opcode或跟踪超限保守保留普通60秒idle，不误当已完成。
  不解析域名、不缓存、不生成DNS答案、不修改DNS字节。
- `main/udp_gateway.c`：32条UDP容量保持；满表优先关闭最久未活动且已完成的DNS
  socket以接纳新flow。无压力时完成DNS保留5秒，待响应DNS和普通UDP仍60秒。
  已排入TCPIP callback的响应持有独立tuple/data副本，回收不会改变其客户端目标。
  DNS新请求复用原客户端端口时，重新加入待响应ID，禁止继续按完成状态回收。
  `main/proxy_config.h`增加4条DNS保留槽位：普通UDP最多28个新flow，已有flow
  继续转发；DNS可使用其余及空闲容量，避免无回复的UDP重试占完全部32条。
  保留槽位的普通UDP拒绝计入独立non_dns_limit，与真正满表丢包区分。
- 入包队列8→16，固定PSRAM增加约12KiB；回包delivery仍8。
  每轮最多处理8个入包；队列仍有数据时select用0等待，防止eventfd通知合并后
  剩余批次又等待100ms。保留固定UDP worker及32个flow socket。
- 10秒累计UDP计数：DNS/UDP443的入包、发送、回复；流表/队列/大小/校验/
  callback/输出等丢包。`output`表示netif接受输出，不证明客户端已收到。
  UDP443只是端口分类，不声称识别了QUIC。
- `main/socks_relay.c`：错误日志增加目标IP/port、state、idle_ms、两方向待发送字节，
  保持TCP搬运/半关闭/窗口/64路容量不变。客户端reset可能是正常取消视频下载。
- `main/CMakeLists.txt` 注册新模块；`tests/dns_flow_harness.c`验证实际生产DNS跟踪模块。
  README增加此记录链接；不修改全局IDF或lwIP patch、GPIO48用途、Wi-Fi配置。

客户端DHCP DNS仍为1.1.1.1，ESP32不是本地DNS服务器。
完整QUIC兼容、IPv4 fragmentation、SOCKS FRAG非零和IPv6未加入本轮。

## 构建与验证

- 7项production IPv4/TCP rewrite回归通过。
- 11项production TCP event-loop回归通过，包括64并发、8MiB echo、背压、EOF、
  网络代际恢复、已关闭写半边保留响应。
- flow_table harness通过512条容量、源端口隔离和关闭保护/hash复用。
- 新DNS harness严格gcc编译通过：并行ID、重试、乱序/重复/无匹配响应、新查询复用、
  8个ID上限/超限、短包、QR错误和非标准opcode。
- prepare_lwip.py check通过，全局IDF未修改。
- DNS回收版`build-android05-fix-final.log`成功，app=0xbfab0；
  最终保留槽位版`build-android05-reserve.log`成功，app=0xbfb40，4MiB分区余81%。
- `flash-android05-fix.log`及最终`flash-android05-reserve.log`记录烧录。
- 板上复测日志：`monitor-android05-fix.log`、`monitor-android05-reserve.log`。应用效果和实际丢包改善须以该日志
  及用户新反馈确认，编译/host测试不能代替Android验收。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check
python tests/test_packet_rewrite.py -v
python tests/test_socks_relay.py -v
gcc -std=c11 -Wall -Wextra -Werror -I main tests/dns_flow_harness.c main/dns_flow.c -o tests/.out/dns_flow
tests/.out/dns_flow
idf.py build
```

客户端无需脚本：重连AP，退出并重开B站，刷新首页、播放视频、拖动进度。
先比较DNS丢包/返回以及交互改善，不以新增带宽参数替代故障定位。

### 修复版首轮板上采样

`monitor-android05-fix.log`启动ELF前缀b34f77e4e与本次build ELF一致，
确认16MB Flash、8MB PSRAM、240MHz、AP+STA和UDP association ready。
19:15:55 DNS=41/41/41，19:16:15 DNS=44/44/44；流表满、队列满和其他
UDP丢包计数均0。完成DNS陆续按5秒空闲回收，采样flow数降到1～3。
当时没有触发满表回收路径（dns_reclaimed=0）；不能把未到满容量的这一窗口
称为满载压测。输入/发送/回包计数也不能证明每条DNS语义答案正确。
客户端具体首页/拖动体验仍待本轮反馈，不声称带宽提高或全部问题解决。
该窗口TCP活动采样峰值35/64，internal_free最低219403 bytes、
PSRAM_free最低7041824 bytes，无watchdog/crash日志。

随后19:16:35流量突发触发9次完成DNS容量回收，DNS=75/75/75、flow满和输入队列满
丢包仍0；验证容量回收路径也在板上执行。普通UDP/443发送62但当时没有对应回复，
说明DNS恢复不等于所有UDP目的都可达，仍须区分上游/目的服务器延迟与APP重试。

到19:18:46 UDP443=234/217/0，整个窗口报文大小/校验/发送/回包校验等计数均0，
但流表再次被普通无响应UDP占满；DNS=103/102/102，仍有一条DNS在此次满表时丢失。
因此增加4条DNS保留槽位并再次构建/烧录。上一版的0丢包不能被描述为长期已经修复；
保留槽位也不保证任意超过32条正在等待响应的并发请求都可接纳。
需要结合用户最终APP反馈确认首页和拖动效果，当前未进行吞吐量对照。

### 用户反馈与剩余延迟

用户随后确认视频拖动已能工作，但首页和浏览器仍偶尔卡住、体感延迟高。
这是DNS回收版的反馈，不应写成整个APP问题已经解决。
最终保留槽位版`monitor-android05-reserve.log`确认硬件启动、AP+STA、association，
DNS=8/8/8，各项本机UDP丢包均0；19:23:18客户端主动离开AP(reason3)，
后续没有客户端流量可用于完整交互验收。
同一版19:22:43仍有UDP控制ECONNABORTED，19:23:04和19:23:24连接握手超时。
因此本轮只修复容量/排队问题，控制链路中断仍然存在，不能报告为长期DNS稳定。
启动AP客户端比STA取IP早时也有network-ready检查拒绝TCP accept的短窗口；
不把这批启动RST归因于64路容量，后续应在AP+STA ready后复测。

从WSL绕过ESP32、仍使用同一SOCKS5进行上游对照：

- 1.1.1.1 DNS两次463.7/467.3ms。
- 114.114.114.114 example.com 23.8ms、www.bilibili.com16.7ms。
  但www.google.com返回104.244.42.197、www.youtube.com返回69.171.235.22；
  地址存在疑点，没有把DHCP DNS贸然改为114。
  对这两个地址带对应SNI通过上游HTTPS分别得到200/证书验证0；
  上游可能按SNI改写目的地址，因此这不能单独验证原DNS地址正确。
- 同一SOCKS访问https://www.bilibili.com/：TLS105ms，首字节226ms，HTTP200。
  这不是B站APP接口、不是AP客户端测量，也不包含ESP32无线/转发开销。

工具`tools/probe_upstream_udp.py`增加--name和A答案输出，方便后续核对上游公共DNS。
现有UDP/443已发出却没有回复的记录仍需上游抓包或实际HTTP3客户端对照；
本机curl不支持HTTP3且没有openssl可执行文件，未把不完整探测称为QUIC验收。
没有永久封禁UDP/443、没有加入分流、DNS缓存、当地DNS服务器或应用层TLS处理。

最终验证的文件列表：main/dns_flow.c/.h、main/udp_gateway.c、main/socks_relay.c、
main/proxy_config.h、main/CMakeLists.txt、tests/dns_flow_harness.c、
tools/probe_upstream_udp.py、README.md、本记录。
最终固件ELF和运行版本应以保留槽位版日志为准；前面的b34f77e4e是中间DNS回收版。
