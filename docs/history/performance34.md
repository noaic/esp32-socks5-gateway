# perf34：复用 lwIP 长包校验和

基线为恢复后的perf29，仅main/packet_rewrite.c的sum_bytes长数据路径变化。长度64..65535使用inet_chksum，转换其网络序取反结果后累加；短IPv4头/伪首部继续原实现。保留原始IPv4/TCP/UDP校验、UDP零校验语义和输出RFC1624增量修改，不跳过验证、不修改PCB。无队列、窗口、任务、无线或上游配置变化；RX copy保留、core locking关闭。

本机pinned esp-lwIP src/core/inet_chksum.c默认算法2，先处理奇地址、再aligned u16读取、最后fold/swap。候选ELF确认长包调用inet_chksum：lwip_standard_chksum在IRAM40386b74，核心循环l16ui/add/指针增加，而原循环使用两次l8ui及多次移位/拼接。字节序转换使用实际lwip_ntohs API，不能直接将native sum当作网络序sum。app807392bytes，无新增静态payload或task。

PATH=/usr/bin:/bin /usr/bin/python3 tests/test_native_checksum.py通过12个测试：提取当前lwIP算法2及wrapper原文，编译生产ESP_PLATFORM分支；与独立Python checksum校验器测试TCP/UDP、增量改写、零边界、坏包、地址偏移0..3、奇偶长度及最大IPv4包。host shim仅适配类型/字节序，未替代校验算法。测试不覆盖真实Xtensa运行或长时间稳定性。

主机微基准：每次100万次1480字节完整TCP parse，3轮交替顺序。baseline aligned中位438.6ns、candidate452.2ns；offset1为445.7/449.0ns，无主机收益。x86编译器及加载方式与Xtensa不同，不能推导ESP必然提速；实机指令更少只是测试依据。host-perf34-checksum-bench.log及临时bench源保留快照。

同期基线三轮4路中位18.65/19.14Mbps；此前22.85/19.78是历史参考，不据此归因本轮。需要同一页面4路对照、内容校验及CPU/驱动错误观察；若无明确收益撤回。板上结果待测。

候选已烧录，flash-perf34-native-checksum.log确认写入hash验证成功。build-snapshots/perf34-native-checksum保存源码/config/bin/ELF、测试和主机微基准；UART monitor-perf34-native-checksum-live.log、页面标签perf34-native-checksum、自动结果bench-perf34-results.jsonl已启用，等待4路测试。仅启动/烧录成功不等同吞吐或长期稳定性已验证。

## 第一组结果与撤回

run muszhvh8-baypsr6zwkm，三轮下载/上传19.69/18.04、19.57/18.26、19.26/19.07 Mbps，中位19.57/18.26。相对最近perf29的18.65/19.14为+4.9%/-4.6%，一组混合变化不能证明整体提速，也不能证明上传必然退步。内容校验通过。8个完整CPU窗口Core0/1 idle40.3%/7.1%、wifi59.4%、tcpip50.5%、proxy40.2%；基线Core1 idle7.45%，没有明确总CPU效率改善。

内部空闲最低102123bytes、最大块57344、proxy栈余量6044；RX held0/peak1、PCB失败0、rewrite parse/mapping drop0、两侧TCP重传0。AP driver mem累计窗口增1525/121264次约1.26%，STA1292/114907约1.12%，无其他驱动错误。驱动背压仍存在，不能将ESP_ERR_MEM直接解释成heap耗尽。未见Guru/assert/watchdog或AP断开。

DNS input_full累计8，第一非零采样06:49:14.419，测速开始06:49:15.106；期间仍8，不能将其归因于测速负载或本轮checksum修改。DNS30/22/22，8次入队丢弃后其余请求均有回复。重连时的DNS短时突发仍是后续延迟优化线索，和带宽候选分开处理。

决定撤回生产checksum修改，恢复perf29；新增正确性测试及候选快照保留。指令数减少未转化为已验证带宽收益，不继续要求用户重复同一候选。后续优先区分无线驱动背压和Core1负载，再选择有证据的单项改动。

回退重新构建后app SHA256=3b300a52715972d478e89ed64f3ad7e6823cff1977e1789e0f6c31e677192f80，与perf33回退后的已验证perf29逐字节一致，app807440bytes。候选专用test_native_checksum.py仅保留快照；复现需先将快照main/packet_rewrite.c与tests/test_native_checksum.py恢复到项目，再执行测试，避免基线无ESP分支时误称验证了候选。

板上回退完成，flash-perf34-rollback.log写入hash验证成功。UART monitor-perf34-rollback-live.log持续采集，测速服务标签perf29-baseline-after34，结果单独保存bench-perf34-rollback-results.jsonl；本轮不再要求测速。复位后的Android需自行重连，长期稳定性没有新增结论。
