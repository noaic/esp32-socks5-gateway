# perf32：16KiB TCP队列对照

基线perf31三轮四路总吞吐中位数下载20.31、上传20.02 Mbps。其client/upstream读取平均7872.5/8044.0bytes，85.58%/94.81%正返回读满8192bytes；send平均6908.5/6270.1bytes、正返回短写12.25%/22.05%。这些数据支持尝试更大批次，但不能证明队列是唯一瓶颈。perf29历史22.85/19.78只作稳定版本参照，不与诊断布局不同的候选混为严格单变量基线。

唯一代码差异：main/socks_relay.c QUEUE_BYTES=8192改为16384。统计、非阻塞、立即发送、队列非空停止读取、EOF drain、公平地每session每轮推进一次均不变。没有额外循环排空或等待凑满，没有新任务；TCPIP/proxy/UDP继续Core1、Wi-Fi Core0。sdkconfig与perf29/31相同，Wi-Fi TX32、窗口23040/snd17280、RX copy、IRAM均不变。

每session每方向PSRAM队列16KiB，64路两方向payload固定2MiB，比perf31增加1MiB PSRAM。metadata内部RAM不变，队列大小仍远低于int/socket长度上限。启动要确认PSRAM分配成功和payload_psram=2097152。更大队列可能减少recv次数，也可能因发送缓冲17280和TCP背压增加短写、延迟或缓存开销；不能只根据满队列承诺提速。没有每连接malloc。

官方ESP-IDF6.1建议少量改变并按实际工作负载验证，并解释lwIP内存、窗口、Wi-Fi参数与吞吐权衡。本项目使用BSD socket，不改为未正式支持的netconn或另写TCP：
https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/lwip.html#performance-optimization

构建build-perf32-queue16k.log成功无warning/error；test-perf32-relay.log 15项生产TCP主机回归通过，其中大数据、背压与双向EOF仍使用实际C状态机。快照build-snapshots/perf32-queue16k保留固件/代码/config/manifest以及perf31基线分析。无需新增镜像常量单测。下一步同条件4路测速，对比平均读/写批次、calls、CPU、driver和内容校验；若收益不明确恢复8KiB并回到perf29稳定版。

状态：构建和烧录完成，写入hash校验通过。app808368bytes，串口monitor-perf32-queue16k-live.log、host标签perf32-queue16k、结果bench-perf32-results.jsonl已启用，待第一组4路测速。

## 第一组实测（06:10—06:11）

run musy3ve2-529g9rnms6l，三轮下载/上传20.17/19.61、21.66/20.11、21.27/19.83 Mbps，中位数21.27/19.83，相对perf31基线20.31/20.02为下载+4.7%、上传-0.9%。内容校验全部通过，待同固件第二组重复；没有宣布保留或整体提速。

覆盖测速的相邻采样差值client_rx positive7427、bytes100689628，平均13557.24bytes（13.24KiB），full68.37%；upstream_rx positive6624、bytes100690062，平均15200.79bytes（14.84KiB），full87.20%。相比perf31 positive12792/12523读取次数下降约41.9%/47.1%，对应数据量几乎相同。upstream/client send positive11362/11395，平均8861.96/8836.34bytes，短写34.63%/41.87%（perf31为12.25%/22.05%），temporary52/18，errors0。读调用减少而短写增加，支持发送背压仍限制进一步减少调用的判断，不证明TCP send buffer是唯一瓶颈。

select差值calls10927 ready10821 timeouts106 errors0。7个完整CPU窗口Core0/1 idle41.4%/4.6%，proxy_tcp38.1%、tcpip54.2%、wifi58.4%，proxy较perf3139.5%仅降1.4个百分点。内部空闲最低101687bytes、最大块最低57344bytes，PSRAM最低5775160bytes，未新增metadata容量。AP发送118942次内存不足250（0.210%）、STA115349次内存不足502（0.435%），其他驱动错误0。PCB失败、rewrite解析/映射丢弃、UDP容量/排队/输出丢弃0，RX held0、peak1；客户端RTO1 fast0、上游0。已有错误计数没有被当成丢包率。

当前决定：待第二组，不进一步扩大到32KiB或同时调整lwIP发送缓冲。若重复后收益仍小或偏单方向，恢复8KiB并移除本轮临时I/O诊断，回到perf29稳定布局。

## 第二组与撤回决定

run musy71qx-f2fzmj5eqq，三轮下载/上传20.98/20.19、18.42/18.77、20.18/19.50 Mbps，中位数20.18/19.50。六轮中位数20.58/19.72，相对perf31三轮20.31/20.02为下载+1.3%、上传-1.5%，无明确带宽收益，不值得为此保留额外1MiB固定payload PSRAM。全部内容校验通过。基线只有三轮，未严格隔离无线状态与布局开销，不能宣称统计显著退步；结论仅是收益不足以保留。

第二组平均client/upstream读取13245.35/15044.21bytes，full63.51%/85.36%。send平均8182.28/8285.95bytes，短写38.23%/44.92%，temporary69/26，硬错误0。8个完整CPU窗口Core0/1 idle42.8%/8.65%，proxy_tcp36.95%、tcpip52.35%、wifi57.0%；更多空闲同时伴随吞吐略低，不能只看CPU百分比宣布效率改善。

AP发送119052次中489次内存不足（0.411%），STA115793次中1030次（0.890%），其他错误0。内部空闲最低107403bytes、最大块最低57344bytes、PSRAM最低5788868bytes。PCB失败0、RX held0、peak1、UDP容量/队列/输出丢弃0，累计客户端RTO1/fast1，上游0。发送压力更高但不直接等同最终丢包；没有内容校验失败。

撤回队列扩容及临时IO统计，恢复perf29源码和配置，重新构建并比对app hash后烧录。既有15项回归均通过；相同代码回退不重复同套测试。下一候选研究lwIP send/TCPIP处理开销，需测量后选择，不直接调大窗口或再扩大到32KiB。

回退完成：最终重编译bin与perf29快照hash一致，flash-perf32-rollback-rebuilt.log hash校验通过。串口monitor-perf32-rollback-live.log和host标签perf29-socket-select恢复，结果另存bench-perf32-rollback-results.jsonl；源码main目录、sdkconfig与perf29快照相同。固定payload恢复1MiB、每方向8KiB队列，无临时IO统计。

回退过程中发现copy2保留旧mtime使Ninja跳过socks_relay.c重编译，首次build/flash实际仍为perf32；hash对照捕获不一致，未将其认作回退成功。刷新源文件mtime后build-perf32-rollback-rebuilt.log确认重编译，app hash恢复5ce1c0de2d1b50d3cd791b395638f061c863274451d8397b3ce16f046a66c0ab，与perf29快照一致。最终使用flash-perf32-rollback-rebuilt.log，保留首次日志供追踪。今后恢复源码后必须使用新mtime或clean强制重编译，并核对目标文件/bin hash。
