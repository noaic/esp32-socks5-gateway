# serial50：串口口令与网络恢复

2026-10-05。在 serial49 非破坏性命令基础上新增 reset-admin、reset-network、confirm CODE、cancel。命令、提示及恢复结果均为简洁英文。保持同一UART连接、原输入限长/限频、WARN日常日志；没有新增任务或改RST/BOOT行为。

## 操作

先执行所需命令，设备会明确说明影响并生成一个8位十六进制确认码：

```
reset-admin
```

保留**本次启动当前正在使用的网络**，清除管理口令，取消配置试运行，提交后重启。在当前AP地址首次访问Web页面重新设置口令。串口不接收新口令，不输出旧口令，不把口令写入命令历史。

```
reset-network
```

恢复**编译默认网络**，保留本次启动当前管理口令/是否尚未首次设置，取消试运行，提交后重启。默认包括STA SSID/密码、AP名称/密码、SOCKS5、DHCP DNS、最大2客户端、192.168.4.0/24；管理地址变为 http://192.168.4.1/。本项目的编译默认STA含本机配置，不是“所有Wi-Fi密码都清空”。当前默认AP名称ESP32-Transparent-Proxy、密码esp32proxy。灯开关与互联网检查偏好保留。

按设备显示的代码输入，不能照抄这里的占位符：

```
confirm <CODE>
```

或放弃操作：

```
cancel
```

确认码有效30秒，只对应最近一次请求，成功/输错/格式错误/过期尝试都会消耗它；取消后、重启后不可再用。新的恢复请求替换旧请求。代码由Wi-Fi已启动后的esp_random生成；参考[IDF RNG说明](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/random.html)。这是防误操作确认，不是串口身份认证；UART是本地物理恢复入口。

确认通过后只排入维护worker，返回“Reset queued. Connections will close.”。维护任务繁忙时返回Busy并取消当前确认；需重新发起，不能把过期代码留到以后执行。提交失败返回“Reset failed ... Settings unchanged.”，不主动重启；成功输出保留/清除范围及重启后的Web地址，随后重启。有效恢复提交不需要Web的180秒二次确认。

## NVS一致性与并发

网络记录v2仍为600字节，未改变schema。恢复生成一个active/previous完全相同的新记录，trial=0，两份bootstrap同步；清口令将两份admin_password全缓冲归零。以后重启不会经previous恢复旧口令，网络恢复也不会通过旧trial回滚撤销。

“保留当前”包括当前未确认的试运行配置：reset-admin固定当前网络；reset-network保留当前试运行管理口令，而不是悄悄找回上一版密码。如果Web已经保存新配置、尚未执行切换重启（trial=1），恢复API拒绝，避免和已安排的重启竞争；重启进入trial=2后可恢复。

原先只有HTTP task写settings；新增串口worker后，init/stage/confirm/rollback/setup/recover统一使用静态FreeRTOS互斥锁。锁内执行NVS操作，未在spinlock中写Flash；网络worker读本次启动稳定的配置，不等待此锁。setup状态使用atomic，pending原有atomic保留。

恢复提交成功后设置recovery_committed，拒绝新的配置写入并清除pending以停掉试运行超时回滚。**运行时网络与登录凭据保持原值直到重启**，不在串口callback修改HTTP鉴权。若HTTP确认先完成，后续恢复仍覆盖两份记录；若恢复先完成，HTTP确认/回滚被拒绝，不会覆盖恢复结果。完整75项回归包含pthread并发确认/恢复20轮。

本机IDF6.1中NVSHandleSimple::commit对有效句柄直接返回ESP_OK，set_blob实际执行存储；host注入commit失败用于验证调用者错误路径，不代表已做真实掉电或Flash故障实验。恢复继续使用单个版本化blob和官方NVS，不擦整个NVS或Flash。所谓清除口令是逻辑重置，不承诺旧Flash页的取证级安全擦除。

## 修改文件与资源

- serial_recovery.c/h：平台独立的确认码生命周期；serial_commands.c/h：新增命令及参数分发；serial_console.c：英文范围提示、硬件随机码和控制队列提交。
- gateway_settings.c/h：静态mutex、bootstrap原子状态、事务化恢复与提交后冻结。
- gateway_control.c/h：恢复操作在现有worker执行，成功才重启，失败可见。
- gateway_state.c/h：恢复提交事件；Web事件中文、串口英文。
- main/CMakeLists.txt、management.html、web_management.c：链接模块、任务名与serial50-recovery标识；Web未开放清口令接口。
- test_serial_recovery.py：确认生命周期、实际C settings恢复与并发；已有settings/control/parser测试扩展。

bin939056字节，serial49为936496，增加2560字节；DRAM data22967不变、bss27928（+112），heap_start3fcb14d0较上一版+112。未新增任务、栈、UART缓冲或日志存储。原6144字节维护栈仍使用；真实恢复时还会临时调用NVS，峰值需要真实执行观察，不能用静态size保证。

STA就绪采样内部free91231、PSRAMfree6984320，对照serial49的91343/6984320，内部少112字节、PSRAM相同。采样不是负载上界。

## 验证

build-serial50.log编译成功、无warning/error；size-serial50.txt保存资源。75项host回归通过，38.203秒，test-serial50-all.log。覆盖确认码大小写/0值/单次/替换/取消/过期边界/非法格式，NVS提交失败、两份密码清除、trial恢复、未首次设置时网络恢复、恢复后冻结写入、重复重启不复活旧密码，以及并发HTTP确认。队列测试覆盖失败不重启、两种成功恢复才重启。

完整管理页JS语法通过。完整回归后又补充参数透传、confirm空参数/多余参数、cancel及reset-network命令解析断言，parser单项重跑通过。

flash-serial50.log hash验证通过，保留当前NVS。monitor-serial50-live.log确认AP192.168.10.1 max2 pending0，STA192.168.1.103，首次自动Internet检测HTTP200。实际串口help列出新命令；reset-admin生成确认提示，输入bad-code后“Invalid confirmation. Reset cancelled.”；reset-network提示AP将变4网段，cancel成功后再提交原确认码得到“No pending reset.”。未发送可执行的有效恢复确认，当前口令与网段未清除或改变。

过期保护及手机原口令/业务验收见文末。恢复后的Web首次设置/默认AP切换/实际NVS重启路径尚未做板上恢复验收，host成功分支测试不能代替硬件验收。

## 使用限制

入口仍依赖正常应用启动后创建的维护worker，不保证早期panic、无法初始化NVS或驱动时还能交互；完整早期安全启动模式未实现。只有RST/EN仍只是重启，BOOT仍为下载用途。单一串口连接拥有者负责日志/命令；烧录前关闭连接，恢复不是重新烧录或擦Flash。

最终验收补录：10:41:33发起的口令确认在10:43:16提交，实际返回“Confirmation expired. Start again.”；板子没有执行恢复重启。用户确认“原口令和上网正常”。取消、错误码和过期保护的真实UART验证均通过；正确确认后真实清口令/切换网段仍留待用户实际需要时执行。

运行381秒时维护栈历史最小剩余1032/6144字节，内部free84155、历史minimum34332、最大连续块32768，PSRAMfree6969556。期间有手机业务，不能把全部动态消耗归因于恢复命令；实际恢复分支尚未采样。

## 测速回归（2026-10-05）

同一Android、192.168.1.10:18080/bench、每种三轮且页面前台完成。单路中位数下载16.43/上传16.25 Mbps，对比web46的15.51/15.31（+5.93%/+6.14%）；四路总吞吐24.49/21.25，对比24.20/21.02（+1.20%/+1.09%）。全部上传校验通过，无测试错误；测速区间WARN日志未新增异常或重启。历史比较不是同场固件A/B，不能把小幅上升归因于串口功能；未观察到吞吐退化。

测速后uptime858秒，AP/STA就绪、Internet pass、failures0，内部free85807、历史min34332、最大连续块36864，PSRAMfree6969452；维护栈历史剩余1032/6144。此为测后采样而非测速峰值连续采样。原始结果bench-serial50-results.jsonl，比较bench-serial50-analysis.json。

## 后续真实恢复验收（2026-10-07）

web51固件已实际执行有效reset-admin和reset-network，用户确认重新设置口令正常、网络恢复后保留该口令且上网正常。当前入口192.168.4.1。详见[板上恢复验收](reset51-validation.md)。本节补充此前仅确认保护分支的覆盖，不改变历史测试记录。
