# 第十轮：UDP 回包诊断，重试实验回退

本页记录当时的实现与命令。2026-10 的目录整理将未编入固件的 `packet_buffer` / `udp_delivery` 实验移到 `experiments/`；下文 `main/` 路径和原命令保留为历史记录，不应直接作为当前操作步骤。当前实验位置与主机验证命令见 [experiments/README.md](../../experiments/README.md)。

**当前固件已恢复第九轮UDP发送路径及队列容量，仅保留错误分类统计；没有启用下面的重试实验。** 实验固件客户端报告下载0.11/上传0.25Mbps和明显卡顿，比第九轮8.07/6.94Mbps严重退步，因此撤回队列扩大和重试接入。两个版本没有严格控制的重复测速，具体归因尚未确定，不把回退动作说成已查明根因。

## 问题与依据

上一版客户端报告下载8.07Mbps、上传6.94Mbps，测速后未断流。相同采集窗口仍有UDP输入队列丢弃7次、输出丢弃11次。旧计数不能区分DNS/普通UDP，也没有记录发送失败的err_t；因此不能断言11次都是Wi-Fi buffer不足，更不能证明所有首页卡顿都由UDP造成。

检查的是本机ESP-IDF v6.1源码：

- `components/esp_netif/lwip/netif/wlanif.c:low_level_output`将NO_MEM映射为ERR_MEM、TX_FAILED为ERR_BUF，其他错误可能为ERR_IF/ERR_ARG；注释说明UDP不会像TCP定时器一样自动恢复本地发送失败。
- 项目本地`components/lwip/lwip/src/api/tcpip.c:tcpip_try_callback`在mailbox满/分配失败时返回ERR_MEM，且不会保留callback；提交成功才保证调用一次。
- 本地`etharp.c`成功保留待ARP报文返回ERR_OK，当前ESP实现队列满拒绝新报文返回ERR_MEM。不能将成功排队的UDP包作为发送失败重复提交。

## 当前修改

`main/udp_gateway.c`仍使用DNS输入8包、普通输入16包、回包8槽位/2个DNS预留及原tcpip callback一次发送。仅增加DNS/普通输入队列满的分类、分配失败/MEM/BUF/IF/down/other与最后IPv4输出err_t计数；没有逐包打印。第九轮cache32/64、TCP满员按需回收等保持。

主机回包实验源码`main/udp_delivery.c/.h`及测试保留供复审，**不在main/CMakeLists.txt的固件编译列表**。`udp_relay.c/.h`保留可选维护接口用于主机集成测试，固件callbacks未设置maintain，select仍使用原100ms上限。若后续重新实验，需审查`patches/udp10-delivery-experiment.patch`并同时将udp_delivery.c加入SRCS；当前只做patch dry-run检查，未重新应用，不建议未定位退步原因就再次烧录实验版。

回退版`build-udp10-baseline.log`构建成功，无编译warning/error，app0xc0c80=789632bytes，分区余81%。ELF SHA256 `789c36da6fa8eec18867c94ef27fe949d636268e05a00230d55ecc317d2d9797`。`flash-udp10-baseline.log`烧录hash校验成功，板上验证采集`monitor-udp10-baseline.log`。

回退后同一服务器客户端反馈**下载7.11Mbps、上传6.35Mbps**，恢复到上一版8.07/6.94Mbps附近。日志在连续10秒采样内记录约8.3MB下载、约8.1MB上传的应用转发增量，支持吞吐已恢复，不能代替SpeedTest准确测量。未继续扩大队列或启用重试。

回退版240秒采集完成，当前输出失败累计MEM=1，其余类别0，重试关闭；DNS/普通UDP输入队列拒绝0。业务采样最低内部空闲164939bytes（约161KiB），最小连续空闲块122880bytes（120KiB），最低PSRAM6878036bytes（约6.56MiB）。未见STA断线、panic/watchdog。这些是短时间业务观测，不保证长期无断流；用户这次只反馈测速，没有明确回复网页是否恢复。

已将当前app.bin/app.elf、bootloader、partition-table及带偏移/校验和的manifest保存到本机忽略目录`build-snapshots/udp10-baseline/`，方便下次实验前保留可恢复的固件。它是含本机配置的生成物，不纳入源码。

## 已撤回的实验

实验保留第九轮cache32/64、64路TCP、512映射、32路UDP association、Wi-Fi参数和上游DNS分流。没有新的任务或定时器，由现有UDP单task/select维护回包重试，TCPIP线程执行pbuf/IPv4输出。下列扩大/重试不在当前固件中启用：

- DNS输入静态队列8→16包，普通UDP仍16包。
- PSRAM回包池8→16个，普通UDP最多占12个，另外4个保留给DNS。固定池启动时一次分配，不按flow增建task或运行时申请应用缓冲。
- 仅pbuf分配失败、ERR_MEM、ERR_BUF重试：间隔20ms，最多4次实际发送尝试（包含首次），回包接受后总保留不超过250ms。mailbox提交失败保留已有数据等待重试，同样受250ms期限约束。每次维护最多处理4包，DNS优先且为普通UDP保留处理机会。
- ERR_IF、AP接口down、其他发送错误直接统计并丢弃；ERR_OK后不重发，不根据远端没有回复去重发UDP请求。不替代DNS/QUIC应用层重传。
- callback尚在mailbox内时，即使STA/AP代际变化或250ms已过，也不把槽位释放给新报文；callback执行时检查原代际/期限并自行释放。避免旧callback访问重用槽位，把旧客户端数据发给新客户端。
- callback可在post返回前执行；成功post后的调用方不再读写槽位。共享状态短临界区保护，current/post/emit在临界区外调用，避免嵌套锁和阻塞。
- select根据最近回包重试期限调整等待时间，不另外用100ms固定轮询延迟20ms重试。DNS与UDP数据、original destination和checksum规则保持。

实际目标ELF类型大小由ESP32-S3 gdb检查：udp_datagram_t=1496bytes，新回包池24488bytes。旧池8×1504=12032bytes，DNS队列额外8×1496=11968bytes；总计额外24424bytes（约23.85KiB）PSRAM。socket/pbuf仍由lwIP动态分配，不能称为完全零动态内存。

## 文件

| 文件 | 修改 |
| --- | --- |
| `main/udp_delivery.c/.h` | 可在主机执行的生产回包池、有限重试、代际保护 |
| `main/udp_gateway.c` | 接入TCPIP输出、扩大DNS队列、发送错误分类 |
| `main/udp_relay.c/.h` | 可选维护回调与select期限、追加诊断计数 |
| `main/CMakeLists.txt` | 实验曾接入，最终恢复原固件源文件列表 |
| `patches/udp10-delivery-experiment.patch` | 保存已撤回的ESP适配器接入改动 |
| `tests/udp_delivery_harness.c` | 受控发送失败、延迟/即时回调、期限与代际测试 |
| `tests/udp_relay_harness.c`、`tests/test_udp_relay.py` | 真实UDP事件循环接入生产回包池的集成测试 |
| `README.md`、本文 | 复现与板上验证 |

## 验证与复现

新增14项回包测试通过严格gcc编译，也通过clang AddressSanitizer/UBSan检查。包括：MEM/BUF/分配失败恢复、成功只发送一次、永久错误不重试、4次上限、mailbox失败保留数据、250ms到期、已排队callback不得提前释放、断线代际保护、DNS预留与每轮公平预算、即时callback、输入存储立即重用。

原SOCKS5 UDP报文4项通过；UDP事件循环14项回归全部通过，见`test-udp10-relay.log`，新增测试让本地输出连续两次ERR_MEM后成功，核对最终只收到一次回包且上游只收到一次请求。主机协议正确性测试不能证明板上速度，因此不以通过测试为由保留客户端实测退步的实验。TCP生产源码本轮未改，沿用上轮15项通过结果，不声称重跑。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
gcc -std=c11 -Wall -Wextra -Werror -I main tests/udp_delivery_harness.c main/udp_delivery.c -o /tmp/esp-proxy-udp-delivery-test
/tmp/esp-proxy-udp-delivery-test
python tests/test_udp_relay.py -v
python tests/test_udp_packets.py -v
python tools/prepare_lwip.py check
idf.py build
```

本机gcc sanitizer动态库缺失，改用已安装clang；sandbox下LeakSanitizer受ptrace限制，在允许的沙箱外执行同一已编译程序后成功，没有关闭内存检查。该固定池无malloc/free，内存检查主要验证越界和未定义行为，不能证明板上长期无泄漏或无并发竞态。

实验构建：ESP-IDF v6.1，无编译warning/error，app0xc11a0=790944bytes，4MiB分区余81%。ELF SHA256 `007b8f00691cef51d3a83fe0b48dfc6de3103e88c949347e566004cb4494ec19`。`build-udp10.log`、`flash-udp10.log`保存已撤回实验的构建/烧录记录，不是当前运行固件。项目本地lwIP patch完整检查通过，全局ESP-IDF未修改。

## 板上观测与判读

已撤回实验的`monitor-udp10.log`确认ELF标识007b8f006、Flash16MB、PSRAM8MB、STA192.168.1.103、AP+STA ready、新队列与4次/250ms参数。初始ready内部RAM186379bytes，最大连续块139264bytes，PSRAM6933152bytes。

300秒实验采集完成，客户端报告0.11/0.25Mbps、延迟明显增加。板上19次IPv4输出ERR_MEM，全部在客户端刚接入后第一个业务采样前出现，alloc/BUF/IF/down/other均0；输出重试计划15次，恢复1包，4包耗尽重试上限，之后没有新增输出失败。DNS79入/79发/78回复、普通UDP44342发/0回复；两类输入队列满、delivery/callback满均0。未见STA断线、panic/watchdog；可见UDP控制连接errno113和TCP握手超时，但不能仅凭这些判断上游端或无线链路根因。DNS响应多数存在，不等于全部APP域名访问正确。

因此已撤回扩大队列和重试，保留诊断以对照。回退实测恢复，说明保留实验不合适；不能仅凭这一次回退确定究竟是队列扩大、重试调度还是客户端/上游瞬时状态导致差异。后续只拆分单项实验，先补AP端ARP排队/Wi-Fi发送拒绝与TCP重传的观测，不继续累加参数。没有观察TCP/无线传输的完整分段/重传统计，无法由RSSI或heap余量排除它们。目标工具链errno.h确认113=ECONNABORTED，118才是EHOSTUNREACH，不套用Linux数字解释板上日志。

每10秒的`local_output failures`中alloc/mem/buf/if/down/other是失败尝试累计数，`last_err`仅记录最后一个IPv4输出失败err_t，不代表最新分配/代际结果。`retries`为输出重试计划数，`recovered`为发生过输出失败后最终提交成功的包数；单纯mailbox恢复不计入recovered。`expired`含迟到callback/等待期限到期，`exhausted`是最终输出尝试仍失败；旧`drops output`计最终丢弃，而不是每次尝试失败。`input_full DNS/ordinary`精确区分两个输入队列的拒绝。

ERR_OK表示协议栈/驱动接受报文，含等待ARP，不等于客户端收到。`recovered`增加能证明本地拒绝被恢复，不能证明公网/无线空口不丢包。若输出失败全零而APP仍卡，继续看DNS关联建立/RTT、UDP443无回包、TCP首响应/满员及上游，不以加大所有队列掩盖问题。

客户端只需Android连接AP，刷新首页/网页、播放拖动；方便时同服务器SpeedTest。保留单客户端限制，测试设备不要争抢AP唯一名额。QUIC支持和长期稳定性仍未完成验证；有界池仍可能在持续超载时丢弃，期限内排队也会增加延迟。
