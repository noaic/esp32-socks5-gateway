# 第十四轮：TCP PCB 容量与关闭状态

上游保留普通Vision，停止新增QUIC探测，不修改Xray或VPS。本轮不扩大
应用64路并发、不改变Wi-Fi带宽/聚合/DNS/UDP队列，不强制重传或周期复位。

## 本机源码证据

`components/lwip/lwip/src/core/memp.c`中ESP专用`num_tcp_pcb`统计全部
分配中的MEMP_TCP_PCB，在达到CONFIG_LWIP_MAX_ACTIVE_TCP时返回NULL，
即使堆仍有剩余。TIME_WAIT和关闭中的PCB在释放前也占这个计数。

`tcp.c:tcp_alloc()`失败后会按顺序尝试回收TIME_WAIT、LAST_ACK、CLOSING，
ESP还尝试FIN_WAIT_2、FIN_WAIT_1，最后按优先级尝试旧活跃连接。这能造成
socket中止。前一轮errno113来自ESP的ECONNABORTED，但尚不能证明每次
中止都由这个容量回收引起。

原上限176；64个应用relay的两端最多128个PCB，加32个UDP控制连接是160。
被动握手、待accept与关闭残留进一步占用PCB。此前日志应用active已达到64，
但没有记录PCB总占用，因此这是合理的待验证边界，不是已证实根因。

空闲回收是socket shutdown/close，由lwIP负责剩余队列和FIN；本轮没有
修改这个逻辑，也不把应用queue为空等同于lwIP unsent/unacked为空。

## 修改

| 文件 | 行为 |
| --- | --- |
| `sdkconfig.defaults`、本机`sdkconfig` | PCB分配上限176→256；socket上限仍208、TCP relay仍64 |
| `main/net_diagnostics.c` | 包装memp_malloc/free，统计TCP PCB live/peak/allocations/failures，保留原返回值、参数和所有权；同时包含12f的age采样修正 |
| `main/CMakeLists.txt` | 加入上述两个GNU链接wrapper |
| `tests/test_net_diagnostics.py` | 实际生产wrapper验证成功分配、NULL失败、NULL释放、非TCP池透传、计数、峰值，以及age竞态 |
| `docs/capacity14.md` | 原因、构建、风险及回退记录 |

观察器不读取或修改PCB，不创建任务/队列，不分配额外数据结构，不改全局
IDF或本地lwIP patch。固定计数为原子变量；日志独立采样不是一致事务。
当前MEMP_OVERFLOW_CHECK=0，编译保护拒绝不匹配的malloc/free ABI。

## 内存与验证

配置只是分配上限，不提前分配80个PCB。本机ELF DWARF给出tcp_pcb=168bytes，
新增80个结构体是13440bytes，不包含分配器开销或更多存活连接的报文/队列。
因此不能把13KiB称为运行时最大内存增量。更多关闭状态同时保留，可能增加
堆使用，需要带载核对internal_free和largest。

- `test-capacity14-observer.log`：wrapper/age回归通过。
- `build-capacity14.log`：ESP-IDF6.1 build成功，无warning/error。
- 项目本地lwIP patch校验通过；未重复未改动的TCP/UDP状态机回归。
- app796192bytes，ELF SHA256
  `804550847dd41d9185224fbf4b0b1704d80cd603f9d40c8d239e50802f25ec05`。
- `flash-capacity14.log`：写入hash校验通过。
- 快照`build-snapshots/capacity14-pcb256/`含manifest、固件及本机配置。
- `monitor-capacity14-live.log`保存本轮被动采集。

## 21:44 实测：容量假设未解释本次卡顿

Android报告21:44卡住。21:44:17/28应用并发36，PCB live147、peak148，
allocations219、failures0，低于原176上限；internal_free约170KiB。
积压flow83目标150.138.144.18:8082，客户端snd_wnd=0、unsent17088，
代理s2c队列10112；上游rcv_wnd=288，是下游积压引起的背压。
两侧driver_rx在两个采样间没有增长，接收安静达到约16秒；TX done继续且
报告成功，buf ref/free平衡。DNS统计34请求/34发送/34回复，UDP443为0。
21:45接收与传输恢复，未记录AP断开。事后WSL ping STA 8/8成功，
但已经错过卡住时段，不能拿它证明现场无线正常。

因此本次不是应用并发或PCB上限触顶，也不是UDP443流量导致。
保留256容量余量及计数观察，不把这项修改宣称为修复。

复现操作与前轮一致：Android移动数据关闭，接入AP，刷新B站首页、播放与
拖动视频两三分钟；卡住时保持连接并报告时间。无需客户端命令或测速。
检查tcp_pcb peak是否超过176、failures是否增加，以及卡顿时的PCB/无线计数。
NULL分配也可能是堆不足，不能把所有failures都当作上限触顶。

回退固件可用`build-snapshots/stability12e-wifi-observer/`的原12e快照；若回退
源码配置，把sdkconfig.defaults与本机sdkconfig的PCB上限恢复176并重新构建。
12d稳定无线快照也保留。快照含本机Wi-Fi配置，不分享或提交。
