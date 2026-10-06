# web47：日常运行与开发诊断配置

维护按钮、双终端管理与转发、DNS配置超时回滚已在web46板上验收，见web46-maintenance-acceptance.md。本版只整理开发诊断，不改变无线配置、TCP窗口、流容量、任务亲和性、配置记录或lwIP patch。

## 两个开关

在 `idf.py menuconfig` 的 `Temporary gateway diagnostics` 中：

- `PROXY_VERBOSE_DIAGNOSTICS`：周期详细UART统计、CPU快照、阻塞流PCB/ARP追踪，默认关闭。
- `PROXY_LINK_PROBE`：旧的AP客户端/STA路由器自动ping，依赖详细诊断，默认关闭。开启后创建3072字节栈的固定任务及按需临时ping会话；不是公网或VPS探测。

日常版两项关闭。需要恢复此前排障能力时开启两项，重新编译；若只需周期日志和PCB追踪，可仅开启第一项。已有sdkconfig须执行reconfigure或menuconfig生成新选项，sdkconfig.defaults只提供默认值，不能覆盖已保存的选项。

Web状态展示实际编译开关；不是可在线切换的运行配置。Web指标、事件、关键错误、用户主动的SOCKS5/DNS诊断和灯功能均保留。后台FreeRTOS运行时间统计设置仍保留，仅停止周期CPU快照及移除其静态数组，不把此次变更称为关闭所有性能计数。

## 实现边界

- tcp_poc：保留每秒mapping过期回收，只移除周期报告、报告用的流遍历和开发PCB trace callback。
- socks_relay：保留select、容量处理和统计计数；关闭十秒详细报告与诊断查询，流水线采样另由既有PROXY_PIPELINE_DIAG开关控制。宿主回归仍保留报告路径。
- udp_gateway：保留所有诊断计数与转发回调，只关闭周期输出。
- cpu_profile：日常版不编译快照及前后采样静态数组。
- link_probe：关闭时init/request是空实现，不创建任务、队列或ping会话；Web手动诊断独立。
- Web API及页面：固件标记web47-daily-diagnostics，显示实际详细诊断和自动ping开关。

启动信息及现有错误日志仍会输出，没有降低全局日志级别。此版不把全部串口输出静默，不承诺吞吐提升；内部RAM和日志开销是主要评估目标。

## 验证

日常版build-web47-daily.log，开发版build-web47-developer.log；两个目录分别build与build-web47-developer，开发配置文件/tmp/esp32-web47-developer-sdkconfig（包含本地编译设置，不应公开）。开发构建必须核对实际CONFIG_PROXY_VERBOSE_DIAGNOSTICS=y和CONFIG_PROXY_LINK_PROBE=y，不能仅凭编译成功认定路径已开启。

60项宿主回归通过，28.246秒，test-web47-all.log；两页完整JS语法通过。初次沙箱测试无法创建socket，随后在允许的本机环境重新运行，最终结果以上述通过日志为准。编译及资源对照、烧录/板测结果随后记录。

两种配置最终编译均成功，无warning/error；日常配置verbose关闭（link_probe因依赖隐藏且未启用），开发配置两项确认为y。日常bin908016字节，开发bin923232字节；web46为922992字节，日常版减少14976字节。

日常DRAM data22951/bss27576字节，开发22967/30392字节；日常heap_start=0x3fcb0960，开发及web46=0x3fcb1470，静态内部余量增加2832字节。IRAM text两者相同。另省3072字节ping worker栈、TCB/队列及按需ping会话；总运行内存节省需板上观察，不用GNU size总计或PSRAM dummy区推算物理RAM。

## 烧录与启动

flash-web47.log烧录成功、hash校验通过。主动复位一次以完整记录启动（已确认配置，无试运行），monitor-web47-live.log。08:55启动确认Flash16MiB、PSRAM8MiB；AP192.168.10.1 max_clients=2、DHCP DNS1.1.1.1、管理pending=0，STA取得192.168.1.103。STA就绪采样internal_free94339字节、PSRAMfree6985860字节，维护startup_delta7468、HTTP8664字节。

启动后未出现旧的周期CPU/网络/TCP/UDP详细报告，现有初始化和错误日志仍输出。两台客户端管理/业务及指标验收待用户确认；没有重跑吞吐，不能据本轮声称带宽改善。

同日上午web46回滚后STA就绪采样内部free88207字节，本版94339字节，多6132字节（约6.0KiB），PSRAMfree均6985860字节。启动样本并非严格全系统原子对照，但与静态和固定任务节省量相符；不能据此推断负载最低堆或吞吐。

源码、构建配置、最终日常bin/elf、编译/测试/烧录日志及资源摘要归档build-snapshots/web47-daily-diagnostics。已保存的口令和网络配置由NVS沿用，未擦除NVS或改配置schema。

## 双终端验收

用户按管理页与另一台正常业务检查步骤回复“两台正常”。串口确认A192.168.10.2、B192.168.10.3均tracked=1；两台status API有成功响应，B HTTP stack_free3628/6144字节。保留现有客户端取消日志和HTTP recv104记录，不据此声称零错误；没有panic/看门狗记录。开关按编译配置关闭，未另行执行本版诊断/灯开关逐项板测或吞吐重测。
