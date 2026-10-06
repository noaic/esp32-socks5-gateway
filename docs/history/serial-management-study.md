# 串口恢复与按需日志：研究结论

2026-10-05。仅研究；未修改运行固件、串口采集器或日志级别，未烧录。用户要求所有串口命令、回复、提示与帮助使用简洁英文。

## 结论

UART0/115200、/dev/ttyACM0 可以共用日志 TX 与命令 RX。板上输入与输出可同时工作；主机只保留一个连接拥有者。未来给现有采集器增加命令发送能力，复用已打开的连接；不能另开第二个 reader，不能为了发一条命令重开设备或切换 DTR/RTS。烧录前关闭所有连接，ROM 下载器运行时应用命令解析器不运行。

日志作为命令有两种含义：开启之后的实时日志可低成本实现；回看过去的完整文本需要日志缓冲，当前没有该缓冲。第一版保留现有64项RAM事件环，不另建完整文本环、不写Flash日志、不新增Web日志流。events展示摘要与计数，不承诺恢复静默期间的逐条文本。

推荐正常启动日志保留，初始化完成后日常级别WARN，输入log info开启实时INFO，log warn回到日常，log off关闭普通应用日志。info保持至显式更改或重启；日志偏好第一版不写NVS，重启返回日常默认，防止一次关闭使后续诊断失去输出。若用户确实要求日常完全静默，可以把默认改NONE，代价是看不到普通错误的即时输出；不是首选。

## 本机IDF6.1证据

- sdkconfig：CONFIG_ESP_CONSOLE_UART_NUM=0，UART115200；LOG_VERSION=1、DYNAMIC_LEVEL_CONTROL=y、DEFAULT/MAXIMUM_LEVEL=3（INFO）；MASTER_LEVEL当前关闭。
- components/log/include/esp_log_level.h：esp_log_level_set("*", level)支持运行时全局设置；不能恢复编译时已移除的DEBUG/VERBOSE。无需为此切LogV2或提高最大级别。
- components/log/src/log_level/tag_log_level/tag_log_level.c：通配符设置更新默认级别、清理tag缓存与列表。只在命令时更改，不在每次worker循环或每条输出中切换。
- components/log/include/esp_log_write.h：esp_log_set_vprintf可重定向普通输出，但回调必须可重入；第一版不引入回调日志环或在回调里执行命令。
- components/esp_driver_uart/src/uart_vfs.c：支持O_NONBLOCK；无driver时从UART FIFO读取，driver模式也支持非阻塞读取。官方REPL安装256字节RX环，TX无环；完整REPL另创建默认4096字节栈任务。尚未选择输入driver方案，不把接口存在当作已集成验证。

官方[日志说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/log.html)区分普通应用日志、bootloader、early/DRAM日志。本项目为LogV1：普通日志级别不覆盖ROM输出、panic/backtrace或所有直接printf。因此log off必须解释为“No app logs”，不能承诺“No UART output”。保留崩溃信息，不通过修改eFuse/ROM打印设置追求绝对静默。

命令回复使用独立输出，不走会被关闭的ESP_LOGI；否则log off以后help也可能不可见。输出按短行发送，不长期持有UART输出锁等待输入，避免挡住panic或其他任务输出。

## 第一版命令建议（尚未实现）

| Command | Help text |
| --- | --- |
| help | Show commands |
| status | Show status and memory |
| events | Show recent events |
| log | Show log level |
| log info | Enable live info logs |
| log warn | Show warnings and errors |
| log off | Disable app logs |
| reboot | Restart device |
| reset-admin | Reset admin password |
| reset-network | Restore network defaults |

回复如“OK”“Busy. Try again.”“Unknown command. Type help.”“Line too long.”。日志打开后保持原IDF英文格式，Web事件中文名称保留，串口事件使用英文名称/稳定编号。status、help均不输出密码。不要接收新密码到命令历史或主机采集日志：reset-admin清除口令后仍通过首次Web页面设置。

破坏性恢复先发短期随机确认token，再要求“confirm TOKEN”；token须绑定操作并过期，不能使用通用yes。reset-admin仅清口令与相关配置副本，不能让trial回滚又恢复旧密码。reset-network保留管理口令，必须说明编译默认STA可能已含当前私人Wi-Fi设置；不是宣称擦除所有用户数据。恢复操作不擦整个NVS或Flash，避免无关偏好与资源信息丢失。

## 资源和集成边界

优先固定短行缓冲（例如128字节）、有界读取与无ANSI重绘，复用维护worker，不新增完整REPL任务。RX环是否安装需对照非阻塞VFS方式测试；短命令不代表可以完全忽略FIFO溢出。

现有worker空闲200ms轮询，但互联网检查内部最长13秒；只在worker最外层读串口会延迟命令处理、可能溢出。集成时需要在检查等待callback里持续接收，复杂操作仅排队/返回Busy，不在callback重入配置写入或重启。若需要实时取消检查，应先给检查加入取消机制，而非另加高频读取任务。只读status输出也要有频率/长度上限，避免UART输出反过来拖慢任务。

恢复入口应尽量早于Wi-Fi启动，不依赖AP/STA成功；目前gateway_control在Wi-Fi start后创建，不能直接接入后宣称能恢复所有启动失败。网络配置与口令恢复涉及活动/备份/试运行记录，需要复用并补齐settings专用接口和NVS提交失败测试。若早期初始化已经panic或NVS无法读取，当前方案未保证仍有交互恢复能力；必要时另行设计有限恢复模式。

## 建议实施顺序

1. 非破坏性help/status/events/log与主机单连接发送，验证乱码、CR/LF、超长行、未知命令、关闭日志后命令仍响应。
2. 验证持续采集、进入下载模式、正常烧录与panic日志保留；测输入环/静态RAM/维护栈开销。
3. reset-admin/reset-network的token确认、trial处理、事务失败和重启加载；单独验收恢复旧密码不会被备份复活。

按需日志有望减少格式化/串口发送开销，但不删除已编译字符串，也不自动释放原任务栈，不能预先保证吞吐提升。当前研究没有对web48做性能或资源变化。
