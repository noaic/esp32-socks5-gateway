# perf30：TCP proxy 不绑定核心对照

基线为已通过普通App验证的perf29，两组六轮四路总吞吐中位数下载22.85、上传19.78 Mbps。第一组21.19/19.66，第二组22.88/20.05；差异提示无线/背景状态有波动，候选需要重复，不能只挑最高一轮比较。

唯一生产修改main/tcp_poc.c：创建proxy_tcp的最后一个参数由1改为tskNO_AFFINITY。仍一个固定任务，8192bytes栈、优先级5、同一个事件循环。TCPIP和UDP固定Core1，Wi-Fi固定Core0。sdkconfig与perf29完全相同，TCP窗口、缓冲、IRAM、RX copy、lwip_select和DNS压力回收均保留。代码比较确认只有这一行不同。

本机ESP-IDF6.1 FreeRTOS task.h定义tskNO_AFFINITY=0x7fffffff，并支持xTaskCreatePinnedToCore参数。官方说明未绑定任务可在两个核心运行，但不会同时在两核执行同一任务，唤醒时调度也偏好当前核心，因此不能承诺平均分配：
https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/freertos_idf.html

既有flow table与enabled状态访问用portENTER_CRITICAL的跨核锁保护，trace busy为atomic；socket和会话状态仍由单一proxy任务拥有。迁移不增加socket操作线程或任务数。cpu_profile日志core=2147483647表示任务affinity，不是实际执行核心；用IDLE0/IDLE1差值评估整体余量，单个任务CPU计数不提供执行核心时间分布。

build-perf30-proxy-unpinned.log构建成功，无warning/error，app807440bytes。test-perf30-relay.log生产TCP事件循环15项主机回归通过。主机回归不模拟FreeRTOS SMP，需板上测速及普通App检查。快照build-snapshots/perf30-proxy-unpinned包含固件、manifest、配置与代码，perf29回退快照保留。

候选风险是Wi-Fi高优先级抢占、跨核唤醒、缓存迁移及共享RAM竞争可能抵消Core1释放的收益，或者增加延迟。先同条件四路测速，再决定重复与App验证；没有新增容量或内存需求。失效或明显退步恢复perf29任务绑定并构建/烧录，不叠加其他优化。

当前状态：烧录成功、hash校验通过；AP+STA就绪，UART确认proxy_tcp core=2147483647，即未绑定核心。测速服务标签perf30-proxy-unpinned、结果bench-perf30-results.jsonl、串口monitor-perf30-proxy-unpinned-live.log已启用，待四路测试。

## 第一组实测（05:50—05:51）

run musxdvsu-mzll8ednb8，三轮下载/上传21.71/20.27、22.86/20.39、22.29/20.48 Mbps，中位数22.29/20.39。相对perf29两组六轮中位数22.85/19.78，下载约-2.4%、上传+3.1%，尚无明确带宽收益，待同固件第二组重复。所有传输内容校验通过。

7个完整CPU窗口Core0/1 idle中位数23.1%/8.4%，proxy_tcp42.9%、tcpip59.0%、wifi62.1%。perf29第二组分别37.2%/4.6%、36.8%、56.9%、62.5%，即解除绑定后另一核心余量被使用，但转发任务总CPU增加、整体空闲减少；可能存在迁移与调度开销，任务统计未记录每核运行时间，不能精确归因。不能把Core1余量释放等同于提升吞吐。

AP发送124809次内存不足174次（0.139%），STA121500次内存不足183次（0.151%），其他发送错误0；较perf29第二组约0.36%有所下降。RTO/fast重传事件均0，RX held0、peak1，rewrite parse/mapping丢弃0、UDP容量/排队/输出丢弃0，PCB分配失败0。活动会话峰值14，内部空闲最低103851bytes，最大块最低59392bytes。两组对照启动与背景连接状态不同，仅按同条件尽量比较，不承诺该差异在实际App复现。

决定：保留候选继续第二组，不同时修改优先级、缓存、缓冲或绑定其他任务。若吞吐收益仍不明确，优先回到已通过App验证的perf29，而不是因为一侧空闲增加便长期保留。

## 第二组、六轮汇总与回退决定

run musxgvn6-ykca460ryr，三轮下载/上传22.39/19.90、22.70/20.56、20.52/20.48 Mbps，中位数22.39/20.48。两组六轮中位数22.34/20.43，相对perf29六轮22.85/19.78为下载-2.2%、上传+3.3%，没有明确整体带宽收益。数据校验全部通过。

第二组7个完整CPU窗口Core0/1 idle中位数25.9%/9.1%，proxy_tcp41.3%、tcpip60.1%、wifi61.9%。Core1余量增加，但总体空闲仍少于perf29第二组，proxy任务总占用增加。只能推断存在调度/迁移等额外开销，未逐项测量CPU周期。AP发送125273次中263次内存不足（0.210%），STA121621次中350次（0.288%）；内部空闲最低107511bytes、最大块最低63488bytes，PCB失败0、RX held0。第二组出现客户端RTO2/fast2，上游0；没有校验失败，不据此称系统断流。

决定撤回解除核心绑定，恢复proxy_tcp固定Core1。不叠加其他优化，以perf29作为后续基线。回退源码必须与perf29快照逐文件一致，重新构建、检查bin hash并烧录，恢复串口及bench标签。15项生产TCP回归已通过；回到相同代码无需重复同一套主机测试。下一方向是根据测量研究收发批次与socket调用开销；目前尚未修改buffer或队列。

回退完成：main目录及sdkconfig与perf29快照一致；重新构建的app SHA256为5ce1c0de2d1b50d3cd791b395638f061c863274451d8397b3ce16f046a66c0ab，与原perf29相同。flash-perf30-rollback.log写入hash校验成功。串口monitor-perf30-rollback-live.log和测速服务标签perf29-socket-select恢复；后续结果单独写bench-perf30-rollback-results.jsonl，不混入历史六轮。
