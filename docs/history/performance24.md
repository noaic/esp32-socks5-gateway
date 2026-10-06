# perf24：flow table 内部 RAM 对照与后续方向

## 本轮修改

仅main/tcp_poc.c：启动时flow_table_t分配由SPIRAM|8BIT改为INTERNAL|8BIT。实际Xtensa DWARF确认flow_t=48、flow_table_t=24840bytes（24.26KiB）。512个映射、128个哈希桶、按本地端口定位回包、连接关闭保护期、锁与过期规则均未变，不把本已有的索引重新实现。

保留perf23会话元数据内部RAM/payload PSRAM、两项lwIP IRAM、RX L2 copy、原无线/窗口/任务配置。此轮只改变flow metadata位置，预计省去逐包PSRAM访问，但需要24.3KiB额外内部RAM；源码没有自动回退到PSRAM以免测量版本混淆。申请失败返回原有NO_MEM启动错误，不继续运行半初始化代理。

## 验证与实测

build-perf24-flow-internal.log构建成功；test-perf24-rewrite.log七项checksum/重写测试通过；test-perf24-flow.log验证独立源端口映射隔离、512容量、关闭/静默保护、hash unlink及槽复用。应用无协议算法改动。

候选快照build-snapshots/perf24-flow-internal/含固件、config/main/manifest（Wi-Fi配置勿分享），flash-perf24-flow-internal.log记录部署；UART monitor-perf24-flow-internal-live.log、自动结果bench-perf24-results.jsonl。页面标签perf24-flow-internal，地址http://192.168.1.10:18080/bench，Android保持前台、暂停其他业务点击开始即可，结果自动保存。

对照perf23六轮中位数12.71/11.55 Mbps。预期收益不确定；必须核对CPU、重传、driver TX mem及内部heap/最大块，优先保证并发稳定。没有显著重复收益就恢复PSRAM，不能为小幅波动固定损失24KiB。此前正常使用日志内部空闲约120—127KB，此轮可能剩约96—103KB，需要实际确认，PSRAM剩余不能抵消内部heap压力。

## 可行的后续优化方向

1. 先做并发和长时间验证：浏览器多标签、视频切换/拖动，记录活动会话、两侧排队、PCB/TIME_WAIT、最小内部heap及最大块，比较用户可见延迟与DNS恢复；单流测速不代表这些场景。
2. 代理事件循环热点：64槽控制元数据已内部RAM；select/socket操作仍有CPU成本，可进一步测每轮扫描/等待/处理耗时，必要时优化活跃槽索引及有界批量转发。不能盲目缩短select timeout（它是无就绪时等待）或加入忙循环，须维护公平性、背压和握手响应。
3. 公网TCP窗口与应用队列：依据实际RTT、吞吐和并发内存测量，再联动窗口/发送缓冲/mbox；扩大每个TCP PCB的缓冲会在64路两端放大成本，8MiB PSRAM并不保证内部内存足够。
4. UDP：保留已经解决的association隔离/DNS流回收，评估高负载DNS排队/重连恢复。不要重启先前造成退步的发送重试队列方案；UDP443上游策略仍保持原样，不新增探测。
5. Wi-Fi：现有AP+STA为同一2.4GHz射频，跨两侧转发有空口成本。早期HT20/b-g等兼容改动已明确退步，不重复盲改。更高稳定带宽若超出该拓扑能力，要以基线证据再考虑硬件上游接口，而不是承诺参数即可成倍提升。

回退：将该唯一分配标志恢复SPIRAM，同步构建/烧录，或恢复perf23快照；保留其它已获重复收益和RX修复。短测不能证明长期稳定、泄漏或64路满负载余量。

app 807056bytes，ELF SHA256 31d3ecbd8cbc124dade48dea8ae871e7ac0e31214df870ae6b6440e418b9cd36。

## 实测与撤销决定

三轮下载/上传Mbps：12.18/10.71、12.31/10.84、11.60/11.33，中位数12.18/10.84；与perf23六轮12.71/11.55相比未确认收益，不能把跨轮差异直接证明RAM位置导致退步。测速全负载内部heap采样104795bytes（102.34KiB）、最大块61440bytes（60KiB），明显减小余量。tcpip39.1%、proxy_tcp44.4%；RX held=0/peak1。候选内容校验通过，但无速度增益证据，不值得牺牲24.3KiB内部RAM，不追加复测。

唯一分配标志已恢复SPIRAM，源码恢复perf23；重新构建/烧录日志build-perf24-rollback.log、flash-perf24-rollback.log。保留两项IRAM、8KiB内部会话metadata、1MiB PSRAM payload和RX修复。候选日志、快照及results.json保留，README继续标记perf23。

恢复版已构建并烧录，Flash hash校验通过，tcp_poc.c与perf23快照一致。自动测速服务已恢复perf23-session-layout标识及其结果文件；持续串口monitor-perf24-rollback-live.log。无需用户再抄结果或为本次无收益实验追加测速。
