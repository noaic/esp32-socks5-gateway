# perf31：TCP I/O批次诊断

以perf29为回退基线，proxy/TCPIP/UDP固定Core1、Wi-Fi Core0。只在main/socks_relay.c增加worker独占的累计统计及read/write路径标签，不改变队列、socket标志、核心绑定、窗口、超时或收发顺序。固定4组9个uint64计数器加4个select计数器，共320bytes；每10秒打印一次，无逐调用日志、计时器或动态分配。统计本身有少量CPU及UART开销，需要同条件测速判断，不能预先保证零影响。

代码已经用fresh_c2s/fresh_s2c立即尝试发送，不存在每次recv后强制等待下一次select。每方向8KiB队列，队列非空时不继续读，保留lwIP背压与EOF drain。本机lwIP sockets.c:lwip_recv_tcp会把当前可用的多个pbuf组合到一次recv，之后只非阻塞尝试追加；不能假定每次recv仅一个TCP段。调整更大buffer只有在实际经常读满时才有依据；否则需考虑发送调用、select/扫描及TCPIP队列开销。

统计路径client_rx/upstream_tx对应上传，upstream_rx/client_tx对应下载。仅有效载荷队列收发计入，SOCKS握手不计。calls包含所有返回，positive是正返回次数，bytes是正返回字节和，small是正返回<=1460bytes（仅分组阈值，非实际MSS测量），full是返回等于本次请求长度，partial是正返回小于请求长度，temporary含EAGAIN/EWOULDBLOCK/EINTR，zero是0返回，errors是其他负返回。recv请求总是8192，故full可反映队列读满；send请求为当前剩余队列，full不意味着8192。recv partial通常正常，不代表丢包；send partial才表示短写。计数器只读取errno，不改变既有错误处理。

select calls包含全部调用，ready是返回>0的调用次数（不是描述符数），timeouts是返回0，errors含EINTR在内的负返回。计算测速前后累计差值、bytes/positive得到实际平均批次，不能直接用进程启动至今平均或把握手及EOF计入成功批次分母。前后10秒采样覆盖窗口会包含少量边界背景流量，应标记这个限制。

15项生产TCP主机回归通过（test-perf31-relay.log），ESP build成功无warning/error（build-perf31-io-profile.log）。主机分支未启用统计，验证状态机不变；板上需要检查四方向字节对应与结果内容校验。未增加镜像宏名称单测。回退快照perf29保留。

部署状态：已烧录且hash校验通过，串口monitor-perf31-io-profile-live.log和测速标签perf31-io-profile已恢复，结果bench-perf31-results.jsonl自动保存。app808368bytes。待一次4路测速；取得诊断数据后才选择下一项优化。

## 实测与下一候选

run musxxbc5-aovz8rp0ted，三轮下载/上传20.80/19.54、19.76/20.02、20.31/20.41，中位数20.31/20.02 Mbps，内容校验全部通过。本组下载低于perf29六轮22.85，不将诊断版当作提速；日志、代码布局与无线状态可能影响结果，尚未隔离。后续候选保留相同计数器，与本轮直接比较。

覆盖测速的相邻10秒采样差值：client_rx positive12792、bytes100704994，平均7872.50bytes、full85.58%；upstream_rx positive12523、bytes100735570，平均8044.04bytes、full94.81%。上传/下载实际数据均约96MiB加少量HTTP和背景流量，收发两侧字节分别一致。读取small比例不足1%，不是大量小数据导致的读取调用。上游/客户端send平均6908.49/6270.11bytes，正返回中短写12.25%/22.05%，temporary26/1，错误0。

7个完整CPU窗口Core0/1 idle40.5%/5.0%，proxy_tcp39.5%、tcpip54.1%、wifi59.3%。RX held0 peak1，PCB分配失败0、UDP排队/容量/输出丢弃0；客户端RTO1/fast1，上游0。仍为CPU接近满载和发送背压共同作用，不能仅根据满队列认定根因。

有依据尝试每方向16KiB队列：减少recv调用的上限可能，但send短写可能增多，实际吞吐与延迟必须验证。只修改QUEUE_BYTES，保持IO诊断和其他参数；64路双方向静态payload从1MiB变2MiB（额外1MiB PSRAM），内部metadata不变。原PSRAM空闲约6.6MiB，有空间；若收益不明确恢复8KiB。不会重新实现TCP或增加每连接task。
