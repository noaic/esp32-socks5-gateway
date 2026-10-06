# perf29：TCP socket select 直达 lwIP 对照（两组测速及普通App通过，保留）

用户表示UPSTREAM_WIFI直连直播不暂停且进入峰值显示4.9M，优先转回带宽优化，暂不继续针对直播协议调参。手机未给单位，不能将4.9M直接当Mbps或MB/s。直连绕过了整个ESP/上游代理路径，不单独证明ESP计算瓶颈。

## 源码与公开依据

本机ESP-IDF6.1 components/vfs/vfs_calls.c:esp_vfs_select()每次调用为vfs_fds_triple及driver_args分配临时堆数据，并从0到nfds逐个读取VFS FD表。lwIP socket使用FD_SETSIZE-CONFIG_LWIP_MAX_SOCKETS的高号区间，少量活动socket也会使nfds较大。

components/lwip/port/esp32xx/vfs_lwip.c将VFS socket_select注册为lwip_select；关闭CONFIG_VFS_SUPPORT_SELECT时socket-only select同样直接调用lwip_select。本项目UDP循环同时等待eventfd和socket，不能全局关闭VFS select；TCP relay只等待listener及两端socket，适合单独直达相同后端，FD编号/集合/超时均不换算。

官方v6.1优化文档明确仅socket select可通过关闭VFS select降低开销，也建议调整任务affinity/优先级及IRAM。这里的局部直达是基于实际后端源码的项目选择，不宣称官方为本项目验证过提速：
https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/lwip.html#performance-optimization

## 单变量修改与架构

仅main/socks_relay.c：ESP分支relay_select=lwip_select，POSIX主机分支仍select；既有调用点改用relay_select。仍一个事件循环、一个socket所有者、相同select超时/异常/EOF/背压。UDP eventfd继续VFS select，CONFIG_VFS_SUPPORT_SELECT仍y。保留live28 DNS压力回收、TX32、Core1 TCPIP/proxy、Core0 Wi-Fi、IRAM/RX copy/固定内存布局及所有窗口/优先级。

固定TCPIP Core1与proxy Core1在四路测试接近满载是调度优化候选依据，后续可单独试proxy未绑定核心；本轮不混入。缓存迁移可能抵消多核收益，不能提前承诺。TCP窗口、队列大小与Wi-Fi缓冲也不在本轮一起调整。

## 本轮基线与部署

live28当前基线run muswk4x5-2kx9ghfwcdm（05:27—05:28）三轮下载/上传20.07/17.52、20.26/18.16、20.95/18.43，总吞吐中位数20.26/18.16 Mbps。采样Core0 idle中位数46.0%、Core1 idle0.6%，proxy_tcp46.0%、tcpip48.0%、wifi53.8%。活动会话约28—31，内部空闲最小94015bytes。CPU采样含窗口边界，非精确逐请求分摊；旧关闭状态和普通应用连接残留也需考虑，不把不同启动状态的全部差值归因于候选。

test-perf29-relay.log：15项真实生产TCP状态机回归通过；build-perf29-socket-select.log编译成功，无warning/error。ELF反汇编socks_relay_run在420111df装载lwip_select地址40387de8，确认ESP分支实际调用后端，不是只修改名字。候选app807440bytes，快照build-snapshots/perf29-socket-select含manifest及baseline/results.json，本机config含Wi-Fi信息勿分享。

flash-perf29-socket-select.log写入hash校验成功；当前板上和源码/build对应perf29候选。新UART monitor-perf29-socket-select-live.log、host标签perf29-socket-select、结果bench-perf29-results.jsonl已就绪，待同条件四路测速。live28回退快照保留DNS修复及TX32。

## 验证与风险

### 第一组实测（05:32—05:33）

run muswra0u-5860b0veqxo，三轮四路总吞吐下载/上传为20.96/19.36、22.97/19.78、21.19/19.66 Mbps，中位数21.19/19.66，比本轮live28基线提高4.6%/8.3%。所有传输内容校验通过，结果已自动保存；仍待同固件第二组重复，不据一组宣布稳定提升。

只统计完整落在测速期间的10秒CPU窗口：基线8个窗口，Core0/1 idle中位数45.75%/0.55%，proxy_tcp46.4%、tcpip48.85%；候选7个窗口，Core0/1 idle38.7%/4.0%，proxy_tcp39.2%、tcpip54.6%。proxy任务下降7.2个百分点，同时lwIP/Wi-Fi处理更多流量。候选活动会话峰值16，基线31，启动与残留连接状态不同，不能把全部CPU/heap差异都归因于VFS开销减少。

前后相邻驱动采样覆盖整个测速：基线AP发送126932次中28次内存不足、STA122333次中8次；候选AP125102次中315次（0.252%）、STA121792次中368次（0.302%），其他驱动错误均0。该返回计数不能直接等同于最终TCP丢包或应用断流，但发送压力确实增加，需要重复观察。候选内部空闲最小101431bytes、最大块最小57344bytes，RX held为0、峰值1，PCB分配失败0。暂不叠加更多TX buffer或任务调度修改。

完整结构化数据及统计窗口保存在build-snapshots/perf29-socket-select/results.json；该文件中的baseline_analysis采用完整CPU窗口，早期baseline_uart_statistics包含边界窗口，比较时使用前者。

### 第二组与保留决定（05:38—05:39）

run muswyapi-8sf2qhd3jg，三轮四路总吞吐下载/上传22.81/20.05、22.88/20.22、23.19/19.78 Mbps，中位数22.88/20.05。两组合计六轮中位数22.85/19.78，相对本轮基线约+12.8%/+8.9%；所有传输内容校验通过。基线只有三轮且残留会话更多，不将该百分比解释为严格隔离的固件增益。

完整CPU窗口7个：Core0/1 idle中位数37.2%/4.6%，proxy_tcp36.8%、tcpip56.9%、wifi62.5%。活动会话峰值12，内部空闲最小108807bytes、最大块最小59392bytes。AP发送125189次内存不足456次（0.364%），STA121923次内存不足446次（0.366%），其他发送错误0。两组间发送压力略增，仍需观察普通App；前后相邻采样未新增TCP RTO/fast重传，RX held为0、peak1，UDP排队/容量丢弃与PCB分配失败均0。rewrite累计mapping_drop=5在此前已出现，本组未增加。

两组测速后暂保留这处小改动，随后完成普通App验证；不叠加队列、缓冲或任务调度调整。已通过的15项主机TCP状态机回归和ESP构建不重复执行，因为固件未改。更长稳定性尚未验证。


本轮已先采集live28基线，完成生产TCP relay主机集成回归与ESP编译后烧录，并完成两组四路验证及普通App验证。不添加只检查宏名称的镜像单测；主机回归无法代替ESP特定lwip_select分支验收。

该路径仅允许socket FD：未来若在TCP事件循环增加eventfd、UART或其他VFS设备，必须恢复通用select或明确拆分等待。既有FD归属与lwIP reference/lifetime由同一worker保持，不能并行close。同一lwIP后端仍有内部扫描开销，VFS消减不保证整体提速。候选失败恢复live28快照，保留DNS/TX32等已验证改动。

### 普通应用验证与最终决定

用户反馈正常使用没有断流、没有明显问题。05:40至05:43:45串口记录转发持续进行，未见重启、watchdog或STA掉线记录；活动TCP会话最高42/64，PCB live最高231、累计peak241/256（含关闭及TIME_WAIT），分配失败0。内部空闲最低91239bytes，最大块最低49152bytes；RX held=0，UDP容量/队列/输出丢弃计数0。

应用期间存在client_exception errno104、客户端侧RTO11/fast4，不能宣称没有单连接RST或重传；上游侧RTO/fast仍0。普通应用阶段驱动内存不足计数仅少量增加，未呈持续上升。用户体感与系统诊断支持保留perf29为当前基线，不代表长期稳定性验收。PCB峰值接近256上限，后续关注关闭状态回收而非直接扩大容量；UDP443仍无回包，本轮不改变上游Vision策略。

下一项带宽候选是仅解除TCP proxy任务的Core1绑定，让调度器使用另一核心余量；先做单变量构建和受控A/B，缓存迁移、Wi-Fi优先级与峰值延迟可能抵消收益，未在本轮实现。
