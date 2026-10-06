# perf20：自动测速上报与 CPU 诊断

## 修改文件与结构

- tools/throughput_bench.html：保持三轮 8MiB 下载/上传、前端校验和展示，新增同源 POST /results，自动报告开始、每轮、完成或失败；计时区间之外上报，5秒超时，保留结果和重试按钮。
- tools/test_stream_server.py：新增最多16KiB的JSON报告端点、结构/数值校验、线程锁保护JSONL追加；保存接收时间、来源、User-Agent、run_id、各轮Mbps/耗时和中位数。存储失败返回500，不能假报成功。
- main/cpu_profile.c/.h：固定64个TaskStatus_t槽位及8个长期任务的前次计数，不新增任务/动态分配；只按已知长期任务handle匹配，避免读取已删除ping任务的名称指针；任务id变化时跳过首样本。
- main/tcp_poc.c、main/CMakeLists.txt：在已有每10秒诊断循环调用CPU采样并编译组件。
- sdkconfig.defaults及本机sdkconfig：启用FreeRTOS运行统计、ESP_TIMER时钟及uint64计数。依据本机v6.1 Kconfig及port.c：xPortGetRunTimeCounterValue()直接将esp_timer_get_time()转为所选计数类型；64位避免32位约71分钟回绕。没有改变窗口、任务优先级、核心、无线参数或队列。

## 测量含义和限制

每个任务的统计为两次快照间累计运行时间增量/墙钟时间，100%表示占满一个核心，两核任务总量可接近200%。IDLE0/IDLE1分别用于估算各核心空闲；wifi、tcpip、proxy_tcp、proxy_udp、esp_timer、link_probe单独打印。任务运行计数在切换时更新，所以10秒窗口的值有边界误差，不是精确逐包CPU分析。

uxTaskGetSystemState()会短暂暂停调度并扫描任务/栈，记录snapshot_us衡量扰动；固定64槽不足时跳过样本，不越界。统计有上下文切换开销，需与rx17速度对照，不能把诊断固件速度直接认定优化成果。

测速服务位于开发主机，不是ESP32 Web管理面。结果路径bench-perf20-results.jsonl，服务日志/tmp/esp32-perf20-peer.log，串口monitor-perf20-profile-live.log。页面仍为http://192.168.1.10:18080/bench，需要刷新；无需用户手动上报数字。报告断网可能无法到达，此时显示重试；关闭页面会丢失未发送报告，不预先保证完全可靠。

## 验证与部署

- Python编译检查通过；临时本机HTTP服务验证start/round/complete/error保存、非法JSON/NaN/缺失轮次/超长请求拒绝、文件写入失败返回500。
- 前端真实脚本在JS模拟环境执行：三轮完成/报告序列、上传校验失败的error报告、上报断网不丢页面结果及重试成功；不代替Android浏览器实测。
- 通过现有Windows SOCKS5 GET /bench确认可达，响应与最新HTML一致。
- test-perf20-diagnostics.log：原驱动/PCB包装契约测试1项通过；test-perf20-rewrite.log：checksum/重写7项通过。
- build-perf20-profile.log构建成功；flash-perf20-profile.log烧录Flash hash验证成功。
- build-snapshots/perf20-cpu-profile/保存固件、配置、main源码及manifest；含Wi-Fi配置，不分享。
- 实际CPU指标及Android自动上报待对应日志验证，未预先归因瓶颈或宣称提速。

回退可恢复rx17源码/配置并重新构建烧录，或使用其既有快照；保留L2_TO_L3_COPY=y。不要只修改defaults而忘记本机sdkconfig。

本次app大小804880bytes，ELF SHA256 a287f544a99b05ceeb614e603a856f35f13934e80d133b81a9b4a973cddf7b84。

## 实际自动上报与CPU结果

23:14:53—23:15:36，Android真实页面POST start、三次round和complete均返回200，文件自动保存成功。三轮下载/上传Mbps：9.65/9.14、9.54/8.72、10.19/8.90；中位数9.65/8.90。下载及上传校验通过，页面始终visible。无需用户抄数。

对应23:15:09/19/29混合上传/下载窗口：Core0 IDLE=14.6%—16.2%，Wi-Fi=38.0%—39.2%，tcpip=45.0%—45.9%；Core1 IDLE=49.4%—51.2%，proxy_tcp=43.3%—47.3%。snapshot耗时840—845us、每10秒一次；统计本身及日志仍有扰动，约1ms快照不能解释所有速度差异。RX held=0、peak=1，无此次接收积压。Core0负载较高，优先考虑TCP热路径IRAM与copy/checksum成本，但平均仍有空闲，不能仅据此认定CPU饱和或无线硬件上限。单核100%口径，别将两个核心百分数简单加成整机百分比。

结果与CPU样本保存在该候选快照results.json。诊断版相对rx17中位数下降约3.7%/2.2%，暂不能区分测量开销与轮间无线波动。后续优化对照必须继续自动记录并同条件复测。
