# perf23：TCP 会话热元数据与 payload 分离

## 修改文件与架构

仅修改main/socks_relay.c和main/socks_relay.h。原session_t含两个内嵌8192-byte数组，与状态/队列长度/时间戳一起由64槽大数组分配到PSRAM；循环扫描访问跨越大步长的PSRAM元数据。候选queue_t改为payload指针+size/offset，session_t保留状态字段，64个session用INTERNAL|8BIT calloc一次分配；独立64x2x8192 payload池用SPIRAM|8BIT malloc一次分配。无每连接malloc、无新task、同64槽及两个8KiB有界队列。

Xtensa ELF DWARF确认queue_t=12、session_t=128bytes，所以元数据8192bytes内部RAM，payload1048576bytes PSRAM。初始化两阶段分配：metadata失败返回；payload失败释放metadata；全部成功才发布global指针，之后init幂等。启动阶段串行调用，worker启动后不改池地址。

accept用c-sessions计算存储槽（不是独立flow mapping index），在清空状态时重新绑定该槽两个固定buffer，重置size/offset/EOF等。finish不释放池，复用下一次accept仍绑定同槽。payload不再整块清零，size=0使未写入字节永不发送；recv成功后才设置size。read_queue容量显式QUEUE_BYTES=8192，不使用sizeof(pointer)，避免转换指针后误把读取容量改为4bytes。host分支也采用同样的分离布局，以让集成测试执行相同queue/accept代码。

未修改flow table的PSRAM位置、lwIP PCB、TCP状态机、协议握手、窗口、核心、Wi-Fi、超时、队列调度。普通及extra IRAM、RX L2 copy及perf20 CPU诊断均保持。

## 验证与部署

- test-perf23-relay.log：15项生产非阻塞转发集成测试通过，覆盖大payload/半关闭、双向慢读背压、握手分片/合并、超时、RST、并发隔离、重连复用、网络重置和容量回收。
- build-perf23-session-layout.log：ESP-IDF6.1构建成功，无编译器warning/error。
- flash-perf23-session-layout.log：烧录Flash hash验证通过。
- build-snapshots/perf23-session-layout/：保存candidate固件/config/main/manifest，含本机Wi-Fi配置，不分享。
- 结果bench-perf23-results.jsonl，host日志/tmp/esp32-perf23-peer.log，UART monitor-perf23-session-layout-live.log。同页标签perf23-session-layout；Android保持前台点击即可，结果自动保存。

与perf22六轮中位数11.91/10.64 Mbps比较，注意本次改变metadata位置及初始化清零开销，不能只归因为一种cache效应。单流benchmark主要测试稳态转发，并发浏览器和连接创建收益/风险需另行观察。尚待本轮真实数据，不预先保证提速。

## 风险与恢复

新增8KiB内部RAM分配及payload指针间接访问；大payload仍PSRAM，不能将1MiB全搬内部RAM。固定池不会按连接数增长/释放，但更多TCP连接的lwIP内存仍会增长，8MiB PSRAM不能弥补内部RAM不足。若无可重复收益或出现坏数据/异常，恢复perf22快照main/socks_relay.c/.h，重新构建烧录；config保持两项IRAM及RX copy。短时正确性测试不能证明长时无泄漏或应用并发稳定。

app 807056bytes，ELF SHA256 d5842e15e0af6c66ea78ab99dd25b9e1352a4c263c0034f367b56e5335e88fbe。

## 第一组自动实测

三轮下载/上传Mbps：12.88/11.37、13.07/11.83、13.07/11.59；中位数13.07/11.59，相对perf22六轮合并11.91/10.64约+9.70%/+8.94%。校验通过，自动报告已保存。

23:41:23/33完整负载窗口proxy_tcp42.4%/42.1%，此前perf22约47.9%/48.3%；tcpip40.3%/39.7%，相比上一版上升，可能反映吞吐提高，不能简单把所有任务占用都要求下降。Core0空闲18.9%/20.3%。RX held=0、peak1，TCP重传0；AP TX mem错误累计6，在完整负载窗口没有增长。内部heap采样132659—136455bytes（129.55—133.26KiB）、最大块86016bytes，PSRAM约6.62MiB。候选固定8KiB内部metadata换取更紧凑扫描，尚待第二组重复，不提前保证长期稳定。

## 第二组复测与最终保留

第二组三轮下载/上传Mbps：12.54/11.48、12.37/11.54、12.39/11.56，中位数12.39/11.54。两组六轮合并中位数12.71/11.55，相对perf22六轮11.91/10.64约+6.68%/+8.54%。两组均改善并通过内容校验，保留当前会话布局及候选固件，不预先保证公网同幅提高。

第二组proxy_tcp43.3%—43.6%、RX held=0/peak1、TCP重传0，内部heap采样134891—136695bytes（131.73—133.49KiB）、最大块86016bytes。AP TX mem错误增加8次（累计6到14/62735次调用），没有观察到持续停流但并未完全消除驱动压力，长期及多连接实用仍需监视。固定metadata8192bytes内部RAM、payload1MiB PSRAM。短测不能证明长期无泄漏或满64路的内存安全余量。

当前仅main/socks_relay.c/.h的数据布局变化保留；配置保持perf22两项IRAM及RX copy、CPU诊断。build-snapshots/perf23-session-layout/results.json、bench-perf23-results.jsonl保存对照数据，README更新当前状态。下一步先考察多标签/视频等应用稳定性及并发内存，再定位下一处热点，避免以增加缓冲替代定位。
