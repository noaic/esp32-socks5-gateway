# perf19：减少AP输入的重复复制

## 上轮结果与本轮基线

rx17同页基线中位数download10.02/up9.10Mbps。
perf18 TX32三轮10.34/9.35、10.57/8.71、9.85/9.16，中位数
10.34/9.16Mbps，仅+3.19%/+0.66%，处于轮次波动量级。没有证明
吞吐收益，恢复TX16。空闲internal_free从rx17约181559降至
TX32约153847，增量约27712B/27.1KiB；不为未确认收益保留此成本。
结果保存build-snapshots/perf18-tx32/results.json，固件快照及日志保留。

## 修改与所有权

- sdkconfig.defaults、sdkconfig：恢复TX16，官方L2_TO_L3_COPY保持y。
- main/packet_buffer.c/.h：准备私有输入buffer和commit/discard所有权。
- main/tcp_poc.c：使用该helper，增加input_buffers reused/cloned计数。
- main/CMakeLists.txt：加入packet_buffer.c。
- tests/test_packet_buffer.py：针对实际生产helper的所有权回归。

启用官方接收copy后，Wi-Fi buffer在进入TCPIP前释放，输入一般已
是普通独占PBUF_RAM。此前透明重写无条件pbuf_clone，重复分配并复制。
新路径只复用ref=1、next=NULL、len=tot_len、非custom、完整
type_internal符合普通PBUF_RAM的输入；其他类型/共享/链式仍clone。
读取本机pbuf.c确认type_internal存储pbuf_type的低字节：PBUF_RAM
含0x0200分配标志，不能直接用完整枚举与8位字段比较。测试使用
一致类型值，避免模拟值掩盖实际fast path永远不匹配的问题。

原IPv4/TCP checksum及格式验证完成、flow mapping取得之后才改写。
借用包失败路径不释放原包，仍由IPv4调用者负责；clone失败路径
只释放clone。成功借用时保持指针/引用，成功clone时归还原包并
替换指针。output pbuf_clone完全保留，避免污染lwIP重传/ARP引用。
没有关闭接收copy，也没有PCB元数据、逐流task或额外分流功能。

## 验证

- test-perf19-buffers.log：真实helper严格编译通过，10000次私有包
  复用/失败路径/提交，以及REF、ROM、POOL、共享、custom、链式
  复制隔离和allocation failure。初次测试仅harness缩进warning，
  修正并以-Werror重跑通过，未改变生产判断。
- test-perf19-rewrite.log：7项原字段/checksum回归通过。
- build-perf19-input.log：ESP-IDF6.1构建成功，无warning/error。
- test-perf19-patch.log：项目本地patch与固定版本一致，全局IDF未改。
- app801440bytes，ELF SHA256
  d072a630758c41561123fba3c767d77b272c621cc3a03f4f6524286cc92c31fd。
- 快照build-snapshots/perf19-input-reuse/保留固件、配置和main源码。

固件部署记录flash-perf19-input.log，UART持续记录
monitor-perf19-input-live.log。实际fast path、吞吐及稳定性仍须
上板验证；主机shim不等同整个lwIP并发/生命周期实测。

## 同页测试与风险

Android重新连接AP，保持移动数据关闭，刷新
http://192.168.1.10:18080/bench ，点开始，回复3轮或中位数。
保持前台、暂停其他下载/视频。下载及上传字节内容会校验。
核对input_buffers reused计数是否增加、RX held仍低、checksum/
mapping/输出错误是否增加、内存是否稳定，不预先保证吞吐提升。

这是rx17基础上的单项数据路径改动（TX32已撤销），不能把
perf18→perf19的全部变化归因于复用，主要与原rx17基线比较。
若出错或不值得保留，烧录rx17-l2-copy快照；源码可用该快照main
和sdkconfig.defaults恢复，并同步移除新增CMake条目/测试。
所有快照含本机Wi-Fi配置，勿分享。

## 实测结果与撤销

三轮下载/上传Mbps：10.02/8.65、9.85/8.98、9.70/8.86，中位数9.85/8.86；相对rx17为-1.70%/-2.64%，未确认速度收益。UART确认28297个输入包走复用、clone=0，parse/mapping drop=0，Wi-Fi RX held=0、peak=1。没有发现本次实验的缓冲积压或校验异常，但短测不代表长期生命周期证明。

生产tcp_poc.c及main/CMakeLists.txt恢复rx17版本，实验辅助源码和测试保留为未编入固件的研究材料，候选快照及results.json保留。继续保留L2_TO_L3_COPY=y、TX16和原无线设置。后续优先测量任务CPU占用、重传与排队，再选单项实验；不把single-hop iperf数字当作AP+STA透明网关保证。

恢复版build-perf19-rollback.log构建通过；flash-perf19-rollback.log烧录及Flash hash校验通过。恢复后ELF SHA256为94aa6194b00b63b90efd79d688f9022636397e4b4dc32dc6e743637304367b4f。tcp_poc.c、main/CMakeLists.txt及sdkconfig.defaults与rx17快照一致，重新编译产物不声称二进制逐字节一致。持续日志monitor-perf19-rollback-live.log。
