# perf33：lwIP TCPIP核心锁对照

回退基线为perf29，应用代码main目录逐文件相同，每方向队列8KiB、固定PSRAM payload1MiB，无perf31临时IO统计。唯一功能配置变化CONFIG_LWIP_TCPIP_CORE_LOCKING=y；sdkconfig.defaults同时明确CONFIG_LWIP_TCPIP_CORE_LOCKING_INPUT=n，输入继续发TCPIP邮箱。保留CONFIG_LWIP_CHECK_THREAD_SAFETY=y。核心绑定、任务栈/优先级、Wi-Fi、窗口和IRAM不变。

本机ESP-IDF6.1组件Kconfig实际提供该选项。lwIP sockets.c:lwip_send调用netconn_write_partly，保留NETCONN_COPY，既有数据复制不消失。api_lib.c经netconn_apimsg到tcpip_send_msg_wait_sem；core lock关时分配消息、post tcpip_mbox并等待sem，开时LOCK_TCPIP_CORE、fn(apimsg)、UNLOCK。tcpip_api_call及其他API也受影响，因此这是lwIP全局API执行模式变化，不只是send一处。不会绕过lwIP TCP状态机。

官方v6.1配置参考：
https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/kconfig-reference.html#config-lwip-tcpip-core-locking

FreeRTOS port使用xSemaphoreCreateMutex，支持优先级继承；sys_thread_tcpip追踪持锁任务，线程安全断言检查当前任务是否持锁。tcpip_thread在等待邮箱时释放锁、处理消息时重新获取；非阻塞socket和阻塞关闭等路径的等待释放由lwIP负责。项目没有手工在socket调用外嵌套锁，避免非递归mutex重入。

审核项目flow table使用跨核critical保护，socket调用不在该critical内。TCP trace和UDP deliver保持tcpip callback，内部原始lwIP操作仍在持核心锁环境执行。输入CORE_LOCKING_INPUT=n避免把收包搬到Wi-Fi调用方；输出可能由持锁proxy任务执行，所以不能再假定所有IPv4 output hook必定在TCPIP线程，但现有表锁及原子计数适配。CPU任务占用会在proxy和TCPIP之间迁移，不能直接比较proxy占用下降来判定效率；必须看总空闲、吞吐、断言、驱动和stack high-water。

构建build-perf33-core-lock.log成功，应用源文件不变，因此不重复相同的POSIX TCP状态机回归；该回归无法覆盖ESP核心锁。ELF反汇编tcpip_send_msg_wait_sem位于4038b6e0，明确sys_mutex_lock→sys_thread_tcpip→callx8回调→sys_mutex_unlock，没有消息邮箱路径，确认编译选项实际生效。反汇编保存在快照。实际板上启动、TCP/UDP及内容校验是本轮必须验收项。

风险是全局锁竞争、任务优先级继承、API执行时机、更深proxy调用栈或内存布局变化可能导致延迟/退步；官方默认关闭，不能预先声称必然提速。无新增payload队列或额外任务，只增加lwIP内部mutex及编译路径相关开销。若出现断言/崩溃/死锁或吞吐无明确收益，恢复perf29 config/defaults并强制重建相关组件、比对bin hash后烧录。不能只复制旧时间戳后假定构建缓存已更新。

基线历史perf29四路两组六轮中位数22.85/19.78Mbps，受无线和背景连接变化影响；候选先同条件4路，再复测和App验证，不将单组差异归因于确定的CPU周期收益。

状态：构建及烧录通过，hash校验成功；app806816bytes。UART持续运行、任务绑定正确、启动期间未见线程安全断言或崩溃，内部空闲112671bytes、最大块65536bytes、PSRAM约6991032bytes，无AP客户端时proxy栈余量记录待负载验证。串口monitor-perf33-core-lock-live.log、host标签perf33-core-lock、结果bench-perf33-results.jsonl已启用，待第一组4路测速。启动成功不等同负载及UDP验证完成。

## 第一组结果与受控回退

run musyke1w-y642z8fwlc，三轮下载/上传19.18/19.33、20.43/19.88、19.50/18.45 Mbps，中位数19.50/19.33，内容校验通过。相对较早perf29六轮22.85/19.78较低，尚未同期A/B，不据此宣称核心锁必然使下载下降14.6%。先回退、补一组当前perf29基线以辨别环境波动。

7个完整CPU窗口Core0/1 idle41.3%/5.4%，proxy_tcp59.0%、tcpip29.4%、wifi58.4%；工作从TCPIP转到proxy，不能把TCPIP占用下降当作整体开销下降。内部空闲最低97727bytes、最大块最低53248bytes，proxy栈余量最低5340bytes，活动会话峰值16，PCB失败0、RX held0、peak1，rewrite解析/映射丢弃0，TCP两侧RTO/fast0，未见assert/Guru/watchdog。

AP发送126382次内存不足673次（0.533%），STA122045次内存不足974次（0.798%），其他驱动错误0。DNS input_full累计3，第一非零采样06:23:12.136，测速开始06:23:12.842；测速前后仍3，不能称测速期间新增3次或证明由核心锁导致。DNS收包35、发送32、回复28，不把未回应都等同DNS永久失效。

决定暂撤回核心锁：收益未体现，继续用perf29作为稳定基线。恢复sdkconfig/defaults新时间戳、重新构建、核对app hash后烧录。候选无崩溃，并非已证实协议实现错误；若回退当前基线同样偏低，说明环境/布局等混杂因素需进一步隔离，而非直接堆叠新参数。

回退已完成，待当前基线测速。sdkconfig/defaults和main目录与perf29相同；build-perf33-rollback.log全量重编译，flash-perf33-rollback.log写入hash校验成功。因CONFIG_APP_COMPILE_TIME_DATE=y且非reproducible，整包hash更新为3b300a52715972d478e89ed64f3ad7e6823cff1977e1789e0f6c31e677192f80。逐字节/分段对照旧perf29：34字节差异仅在首段app descriptor的compile time/ELF摘要，另外33字节是校验尾部；其余DROM及全部DRAM/IROM/IRAM/RTC段一致，实际代码未变。不会误认这种元数据更新为构建缓存未恢复。

UART monitor-perf33-rollback-live.log恢复，host标签perf29-baseline-after33，结果bench-perf33-rollback-results.jsonl单独保存。采集当前同条件4路基线后再决定下一方向。

## 同期perf29恢复版基线与修正结论

run musyrxd1-3c2vcgaqztx，三轮下载/上传18.09/19.29、18.65/18.10、18.84/19.14 Mbps，中位数18.65/19.14。相对这个同期基线，perf33核心锁19.50/19.33为下载+4.6%、上传+1.0%。此前较早22.85/19.78不能作为证明核心锁退步的依据：恢复原代码/config也没有恢复早先下载成绩。单组小幅收益不足以证明核心锁稳定有效，保持已验收的perf29，不重新开启或叠加参数。

8个完整CPU窗口Core0/1 idle41.55%/7.45%，proxy_tcp38.85%、tcpip49.75%、wifi58.2%。内部空闲最低102999bytes、最大块最低59392bytes，AP发送127406次内存不足876（0.688%），STA123007次内存不足1576（1.281%），其他驱动错误0。相同perf29驱动压力比早先明显增加；这说明运行状态存在变化，但不能单独证明无线干扰、温度或上游Wi-Fi问题。RX held0/peak1、PCB失败0、rewrite解析/映射丢弃0。DNS20/20/20、UDP入队/容量/输出丢弃0；客户端累计RTO1/fast1，上游0，校验全通过。

自行完成WSL经现有Windows SOCKS到同一WSL bench的8MiB传输（绕过ESP和无线）：下载0.093319/0.099251秒，约719.1/676.2Mbps；上传0.069628秒约963.8Mbps，服务端SHA256匹配。只说明这个本机代理/测速服务路径有明显余量，不是VPS出口或ESP STA链路测速。小传输含虚拟机/系统缓冲，第二次下载与上传并行，不能作为精密硬件线速结论。

后续采用更接近时间的交替A/B并观察驱动压力，避免把长期波动误认成固件优化。当前Wi-Fi参数保持不变。更可控的代码方向是检查packet_rewrite输入校验和CPU成本、优化相同语义的计算/代码放置；不能跳过TCP校验或撤回RX copy来换速度。尚未实现该候选，本轮无更多烧录。
