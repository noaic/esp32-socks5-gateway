# 第二轮容量与连接重置诊断

## 用户反馈和判断

上一轮 32 路优化版用户测得约 361K、22 秒；旧版本约 249K、32 秒。
这是约 45% 的速度改善，但不是严格同条件多次平均，不能归因到某一个参数。
YouTube 1080p 可播放，用户页面统计 connection speed 3000、buffer约20秒。
多个标签仍出错，错误代码为 ERR_CONNECTION_RESET。

monitor-perf01.log 显示活跃 TCP 多次达到 32；同一时期两侧RSSI约 -33~-40dBm，
metadata未再报失败。容量等待客观存在，但满额暂停accept本身不等于
发送RST，因此不能把 ERR_CONNECTION_RESET 全部解释为容量。
前版 error-close 只有 reset=1，缺少发生在哪一侧、哪个操作的证据。

## 修改

- main/socks_relay.h/.c：活跃TCP上限32→64；仍为单个固定event loop和有界PSRAM缓冲。
  容量满时等待空位，没有逐流task。错误日志新增 operation/errno，区分
  client/upstream recv/send/exception、handshake和idle timeout。
- main/proxy_config.h：MAX_TCP_FLOWS同步为64，full4tuple保护映射保持512。
- sdkconfig.defaults、忽略的sdkconfig：sockets176、active TCP PCBs176，
  可容纳64*2 TCP、32UDP、listener/control以及余量。
  FREERTOS_HZ=1000：来自本机官方iperf通用defaults，减小调度/timer tick粒度；
  未证明原来的100Hz就是单流吞吐瓶颈，不承诺更改后的倍率。
- CMakeLists.txt：全组件FD_SETSIZE=256，lwIPsocket offset=80，统一app/lwIP/VFS ABI。
  全局IDF和toolchain未修改，原透明packet patch不变。
- tests/test_socks_relay.py：测试64个同时打开的会话、第65条容量等待并获得空位、
  客户端reset和网络down/up恢复；去除非fragmented握手中的人为睡眠。
- README.md、本记录：更新最新参数和验证状态。

## 验证与复测

9项生产TCP event loop回归通过，包括64路并发和满额恢复。
上一轮6项双向TCP字段/checksum测试和512映射harness仍适用，这轮未改对应实现。
ESP-IDFv6.1 build成功，无warning/error；镜像0xbe520，4MiB app分区余81%。
Flash hash校验通过。启动与实际错误日志采集到 monitor-perf02.log。

仅复现网页错误：客户端重新连接AP，打开之前报 ERR_CONNECTION_RESET 的网页，
回复网址与复现时间。无需重复测速。根据 operation/errno，区分：
上游TCP断流、客户端取消、握手失败、idle超时和网络事件。
client_receive ECONNRESET只说明收到客户端RST，可能是取消请求，
不能仅凭该日志认定是当前页面失败的根因；必须与复现时间/远端关联。

## 64 路版本实际采集结果

用户反馈多标签出错，与特定网址关系不大。monitor-perf02.log 启动后约383秒内：

- 统计采样的活跃连接峰值54/64；没有accept容量/网络/mapping拒绝日志。
  统计每10秒采样，不能排除采样间的瞬时峰值，但尚无直接证据证明此版容量耗尽。
- 20次upstream_exception errno=104。已对照本机lwIP的sockets.c与err.c：
  select异常集合来自socket错误事件，SO_ERROR的104对应ERR_RST/ECONNRESET。
  这说明ESP32到上游SOCKS5的TCP连接收到重置；不能据此区分Xray自身、其出口或中间网络的原因。
- 另有客户端reset、3次SOCKS握手超时和52次120秒idle超时。
  客户端reset可能是浏览器取消；idle超时是当前ESP32策略主动结束闲置连接。
  不能把所有closed reset=1计数都当成网页失败计数。
- STA和客户端RSSI约-33~-46dBm；未采到Wi-Fi disconnect、network/client reset、panic或watchdog。
  最新空闲internal RAM约227KiB，最大连续块143360字节，PSRAM约7MB；
  没有当前内存耗尽证据，也不代表已经完成长期泄漏验证。
- UDP ASSOCIATE多次失效重建，包含5秒重试等待和握手耗时。
  这会造成DNS转发空档；目前UDP错误日志不足以区分控制TCP关闭、错误与握手超时。
  DHCP仍下发1.1.1.1，公共DNS流量依然需要经过SOCKS5 UDP路径，换DNS不能绕过该问题。

已请用户查看同一时期Xray日志，关联upstream reset/timeout/EOF与出口错误。
64路版本连接重置根因仍待定位，尚无新的受控吞吐测量。
一条连接的速度不能只由并发上限解释；目前尚无AP+STA LAN baseline来量化无线空口与
CPU/socket转发的占比。512保护期表、64活动TCP、32UDP等仍然有界；不宣称完整软路由能力。

## Xray 两端日志关联（diagnostics03）

用户提供07:16:28/29的Xray日志：read/write tcp
192.168.1.2:7897->192.168.1.103:63855/63856，分别超时与连接被对端强制关闭。
该socket的对端是ESP32，不是VLESS服务器；日志中的VLESS调用链不能改变socket端点的含义。
第二条也可能是ESP32因客户端RST、idle/handshake超时或网络事件结束连接的结果。
既有ESP32日志没有上游源端口和采集墙钟时间，无法把这两条精确对应到flow。

为降低多终端Xray日志的筛选成本，修改：

- main/socks_relay.c：accepted/CONNECT ready/error/closed均记录upstream_port。
  该值来自getsockname，是ESP32->SOCKS5连接的本地TCP端口，与Xray记录一致。
  同时避免select失败或网络切换后，用无效的exception集合覆盖原始错误原因。
- main/udp_gateway.c：记录UDP控制TCP的upstream_port、失效state/operation/errno；
  区分control_eof、control_exception、control_receive、握手超时/拒绝与网络变化。
  connect errno在getsockname前保存，避免诊断查询影响连接状态判断。
- tools/uart_capture.py：新增--timestamps，按串口完整行写入本机墙钟接收时间。
  这是接收时间，存在UART/调度延迟，不等同于板上事件的精确墙钟时间。

9项TCP回归通过；build-diagnostics03.log编译成功，镜像0xbe710，app分区余81%。
flash-diagnostics03.log烧录成功并通过hash校验；带时间戳采集保存到monitor-diagnostics03.log。
该版补充诊断，不宣称已经修复连接重置；没有更改容量、缓冲或超时策略。
串口命令（由开发机执行，不需要AP客户端执行）：

```bash
/home/user/.espressif/tools/python/v6.1/venv/bin/python tools/uart_capture.py --no-reset --timestamps --seconds 3600 --log monitor-diagnostics03.log
```

Xray日志先筛选192.168.1.103，再按端口和时间关联串口error/closed以及CONNECT remote。
无需导出所有终端日志，无法把历史63855/63856补溯成准确flow。
