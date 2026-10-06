# perf26：TCPIP Core 1 单参数对照

## 修改与依据

sdkconfig.defaults及本机sdkconfig：CONFIG_LWIP_TCPIP_TASK_AFFINITY_CPU0关闭、CPU1开启，生成的CONFIG_LWIP_TCPIP_TASK_AFFINITY=0x1。先读本机v6.1 components/lwip/Kconfig及port/freertos/sys_arch.c：sys_thread_new用xTaskCreatePinnedToCore(...CONFIG_LWIP_TCPIP_TASK_AFFINITY)创建线程，适用于TCPIP和Ping。Wi-Fi仍CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y，TCPIP优先级18，proxy_tcp/udp仍Core1优先级5；窗口、队列、IRAM、RX copy及perf23存储不变。

sdkconfig里的旧CONFIG_TCPIP_TASK_AFFINITY*是本机sdkconfig.rename明确映射的别名，修改时去掉旧CPU0/整数值让它们随现代Kconfig再生成，不同时留下互相冲突的两个选项；没有根据旧博客猜名称。

main/cpu_profile.c仅在既有CPU统计日志后增加core=字段，使用本机IDF FreeRTOS idf_additions.h声明的xTaskGetCoreID查询长期任务的固定核/NO_AFFINITY；CONFIG_FREERTOS_SMP未启用，该API适用。已知长期任务handle不因短命ping删除而变成无效名称指针。不增加任务/采样频率，不改变转发算法。

perf25原单路12.69/11.07 Mbps，四路15.73/13.65；四路完整窗口Core0忙约89.5%，Wi-Fi45.3%、TCPIP43.5%，Core1忙约35.1%、proxy_tcp33.1%。候选分开Wi-Fi与TCPIP，后者与proxy共享Core1，以利用当前空闲，但可能让高优先级TCPIP压住proxy低优先级工作，所以不提前保证提升。

## 验证与部署

build-perf26-tcpip-core1.log构建成功；本机生成配置确认TCPIP=1、Wi-Fi=0。flash-perf26-tcpip-core1.log Flash hash验证通过。算法不变，不新增镜像单测；沿用已有15项relay及checksum/flow回归，本轮以编译、生成配置、真实任务core日志及单路/四路数据验证。

build-snapshots/perf26-tcpip-core1/保留固件/config/main/manifest，含Wi-Fi配置，不分享。UART monitor-perf26-tcpip-core1-live.log，自动结果bench-perf26-results.jsonl，host日志/tmp/esp32-perf26-peer.log。

Android重连AP、保持移动数据关闭、刷新http://192.168.1.10:18080/bench确认perf26-tcpip-core1，先单路后四路，保持前台、停止其它业务，结果自动记录。沿用三轮、每流8MiB/方向，四路是总吞吐，不能当作单流成绩。等待真实数据、CPU核验证及排队/内存/RX/重传记录。

## 风险与恢复

固定Core1不等于自动负载平衡；任务抢占、跨核RX投递/socket调用及缓存局部性可能改变。Wi-Fi/AP+STA同射频空口成本仍存在；移核不会改变物理链路或上游。更高吞吐也可能放大发送driver内存压力，必须看内容校验及错误计数，不只取最好一轮。

如果无可重复收益或稳定性退步，恢复TCPIP CPU0（同步defaults和sdkconfig及其生成别名）重新构建/烧录；已保留的perf23快照可恢复。新增core日志可保留观察，恢复时不能只修改defaults忘记本机sdkconfig。短测不等于长期并发稳定。

app 807104bytes，ELF SHA256 edb66c58969823a5c71307e24596986180d1007123e7f2ea507ca221f2fbddb8。

## 单路及四路自动实测

单路三轮下载/上传Mbps：13.72/12.85、14.39/13.02、14.80/13.40，中位数14.39/13.02，对比perf25 12.69/11.07约+13.4%/+17.6%。四路三轮总吞吐20.03/17.92、20.77/17.59、20.41/17.56，中位数20.41/17.59，对比15.73/13.65约+29.8%/+28.9%。均内容校验通过、visible、自动报告完整。日志确认wifi core0、tcpip core1、proxy core1，候选实际生效。

完整10秒CPU窗口按测试start/end筛选，结果及CPU汇总保存在候选results.json。4路Core1接近满载（部分窗口IDLE1约0.1%），TCPIP及proxy共享该核，Core0 Wi-Fi约六成负载。驱动TX mem错误明显增长，00:12:14累计AP3191/157246、STA2128/153815约2.0%/1.4%，相比此前对照不能称为低错误或稳定性已证明；TCP重传observer仍为0，RX held=0/peak1，PCB峰176/failure0，未发生资源池枯竭。

测试期间出现182.40.146.99:8082等非benchmark连接，客户端窗口0造成局部流背压，不能把该流6400bytes队列直接说成测速peer堵塞，也不能将本轮视为完全孤立业务。固件暂留，待正常网页/视频两分钟观察再决定；增速不自动覆盖稳定性风险。更高负载下重新评估TX缓冲有新的依据，但本轮不混入第二项参数。

## 实际应用反馈及最终保留

用户正常刷新B站首页、播放拖动、打开网页后反馈：没有卡住或明显断流，轻微可感延迟基本不影响。约00:15:25—52活动会话最高50、内部heap采样122451—124171bytes（119.6—121.3KiB）、最大块77824bytes，PSRAM约6.35MiB；RX held=0/peak1。STA TX mem累计保持2128，AP从3224缓慢增至3232，满负载压力在正常业务下降，未观察到全局转发停止。局部客户端窗口0流仍属背压，不以该单流等同全局卡死。

保留当前perf26（TCPIP固定Core1，Wi-Fi Core0，proxy固定Core1），源码/defaults与板子一致。更新README当前固件及单路/四路成绩。真实应用仅短期反馈，仍不能证明长时间无泄漏或64路满负载稳定；四路Core1空闲中位数1.4%、最小0.1%，满负载TX mem约1%—2%仍需后续优化。

下一轮可以围绕有依据的发送缓冲压力或代理调度做单项实验；不能一边改核心、一边扩窗口/缓冲后把全部增益归因于一项。早期TX32在低吞吐下无确认收益，此时并发吞吐/驱动错误不同，若重测必须记录新对照及内部RAM成本，不将旧结论当作永远成立或反过来跳过验证。
