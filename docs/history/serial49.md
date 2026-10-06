# serial49：基本串口命令与按需日志

2026-10-05。第一步只实现非破坏性 help/status/events/log；不增加串口重启、口令清除或网络恢复操作。后者需要另行完成确认与 NVS 活动/备份记录一致性。所有命令、回复、帮助、串口事件名均为简洁英文，Web 仍为中文。

## 使用

UART0，115200；原设备 /dev/ttyACM0。普通串口终端或 idf.py monitor 中输入以下命令，每行回车；无需专用上位机。

```
help        Show commands
status      Show status and memory
events      Show last 8 events
log         Show log level
log info    Enable live info logs
log warn    Show warnings and errors
log off     Disable app logs
```

运行初始化完成后保留约10秒启动信息，再输出 Ready 提示并默认切WARN；用户提前发出的log命令优先，不被默认切换覆盖。INFO 开启后持续至更改或重启；偏好不写NVS。完整固件启动/ROM/panic路径保持原配置，log off仅抑制普通应用日志；命令回复用printf/puts，关闭日志也可读取。

events显示现有64条RAM事件环中的最新8条，按先后顺序输出时间/英文名称/代码/合并次数，不增加完整文本日志缓存。状态不含密码。静默期间完整文本无法补回，事件环重启清空。

## 架构与资源

serial_commands.c/h 为无平台依赖解析器：96字节行缓冲（最多95输入字符），ASCII、CR/LF/CRLF、退格、空行；未知/非法/超长行明确报错。非法与超长输入丢弃到行尾，不执行残留前缀。每秒最多执行一个命令；突发重复只发一次Busy，限制输出对UART及worker的占用。

serial_console.c/h 复用gateway_control现有6144字节栈、priority2任务，空闲约200ms轮询。等待DNS/HTTP的callback也调用同一poll，不新增任务或并发RX消费者。每次最多读64字节。UART0安装256字节RX环，保留原baud/pins、原stdout VFS，无额外TX环。没有使用完整REPL、ANSI重绘、历史记录、任意内存命令或日志重定向回调。

本机IDF6.1使用uart_driver_install/uart_read_bytes(...,0)与esp_log_level_set("*",level)，当前编译最大INFO，不提供不存在的DEBUG内容。参考[官方日志说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/log.html)。不能把运行时静默视为编译删除日志字符串。

bin936496字节，较web48的918752增加17744字节；DRAM data22967/bss27816，分别+16/+144。_heap_start从3fcb09c0移至3fcb1460（2720字节），IRAM text由0x1bfa3增至0x1c9cf（2604字节）；除data/bss外还需计入新增代码对内部内存布局的占用，不能仅报“160字节RAM”。

板上STA就绪internal_free91343、PSRAMfree6984320；web48对照94199/6985860，分别少2856/1540字节。maintenance startup_delta7604，对照7468。新增驱动部分分配可落入PSRAM，启动采样非原子、非负载峰值。10:20:56 status记录internal_min54964、largest47104；维护任务已记录stack_free_min1176/6144，即最大已观察使用4968字节。没有测试全部网络压力与输入组合，不把这项数值视为数学上界。

## 主机采集与烧录

修改现有tools/uart_capture.py，新增--interactive，从stdin读取短ASCII行，使用已打开的UART发送；不打开第二串口。--info-logs在连接后及每次Ready提示后发送log info，适合Codex持续采集。tools/uart_passive.py新增有界非阻塞write，沿用同一fd，不触碰DTR/RTS。

例如（先停止其他串口占用者）：

```
python tools/uart_capture.py --no-reset --timestamps --seconds 0 --interactive --info-logs --log monitor-live.log
```

普通终端可直接键入命令。Codex使用PTY会话，通过write_stdin发送同样命令；主机采集器写文件并持续读取。设备重启后需重新开启INFO，--info-logs可自动处理。不要并开另一串口终端/monitor/esptool；烧录前关闭采集器，ROM下载器工作时应用解析器不运行。

--no-reset只避免主动操作复位线；适配器/内核首次打开仍可能改变线状态，持续持有一个连接更可靠。当前板测采集时主动复位一次以记录启动，没有为了逐条命令反复打开设备。

## 验证与文件

新增main/serial_commands.c/h、serial_console.c/h；修改gateway_control.c的wait callback和UART初始化、app_main.c的启动完成通知、main/CMakeLists.txt、Web固件标识。工具修改uart_capture.py与uart_passive.py；新增tests/test_serial_commands.py与test_uart_capture.py，control测试补串口callback适配。研究提案见serial-management-study.md。

73项宿主回归全部通过，37.917秒（test-serial49-all.log）；实际C解析覆盖分段、CRLF、退格、二进制/ESC非法、超长、限频、未知命令及无恢复命令。实际采集器在伪UART上验证被动读取、同连接命令、超长主机输入拒绝与Ready后再次开启日志。工具Python语法检查通过。

build-serial49.log成功，无warning/error；size-serial49.txt保存资源；flash-serial49.log写入hash验证通过，保留NVS。monitor-serial49-live.log板上确认AP192.168.10.1、max2、pending0，STA获得192.168.1.103，自动Internet检查HTTP200成功。

真实UART验证：Ready默认warn；help为英文；status正常且Internet=pass；log off返回OK后events仍给出最近8项英文摘要；log info返回OK后重新收到普通INFO日志。log查询与限频的板上覆盖随后补录。没有故意触发panic，崩溃路径只确认配置未改。Web/手机业务由用户验收；本轮未重跑吞吐。

## 限制与后续

复用worker意味着命令到达不是实时中断执行，有200ms左右空闲轮询粒度及输出时间；串口没有完整shell编辑/字符回显，终端可开启local echo。events只显示最新8项，Web环容量仍64。RX环并不保证无限输入无丢失，接口用于短低频管理命令。

控制任务仍在Wi-Fi start后创建；这组非破坏性命令不能保证在所有早期启动panic/NVS损坏情况下可交互。以后恢复功能必须同时处理配置备份、trial及口令bootstrap，不直接擦全部NVS。第一版主机交互兼容POSIX/WSL stdin；没有为Windows Python stdin另做适配，Windows用户可以用普通串口终端。

最终板测补录：log查询返回info；连续发送log/help/status只执行首条并输出一次“Busy. Wait 1 second.”，未大量输出。随后正常status仍可读取；10:25:23第二次自动检查HTTP200成功，10:25:24显示内部free77731、历史最低29076、最大块31744，维护栈最低剩余仍1176。该阶段存在手机正常业务和历史连接，不将最低堆归因于串口或单次检测。未观察到新增panic/看门狗/重启。用户确认管理页与上网“都正常”。

控制任务宿主测试另外断言两种探测的等待callback确实轮询串口，单项重跑通过；未把实际UART命令与网络等待恰好重叠当作已覆盖。测试后通过命令切回WARN，保留单一UART采集连接；需要详细排障时可在该会话发送log info。
