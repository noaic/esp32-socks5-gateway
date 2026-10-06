# diag36：设备自行测 STA＋SOCKS 端点

沿用diag35 AP本地服务，新增POST /sta-bench：同一个httpd固定任务同步执行3轮8MiB下载/上传，通过现有192.168.1.2:7897 IPv4 NO AUTH CONNECT到192.168.1.10:18080已运行的WSL测试服务。两侧大数据只经过STA，无AP数据转发，不创建第二任务、不增加payload缓冲、不更改路由/Windows/VPS/Xray；这不是纯STA或公网速度。原AP页面URL192.168.4.1:18081/bench不变，新增上游测速按钮。

计时从建连前到整包及回复校验完成，包含TCP/SOCKS握手、HTTP header逐字节读取、全部内容检查，并记录setup_ms。下载memcmp完整0..255周期内容，上传由既有主机SHA256校验并返回matches_8m。仅支持这个控制服务的HTTP/1.x Content-Length响应，不实现通用HTTP客户端、TLS或domain ATYP。header上限1024bytes；socket收发5秒timeout、非阻塞connect/select5秒、每方向60秒deadline。浏览器400秒超时覆盖最坏6个方向等待，界面前台无其它测试。失败输出phase=error，不记录虚假完整成绩；单次诊断HTTP handler执行期间AP测速请求排队，因此前端禁用全部测速按钮。

复用24KiB PSRAM pattern/receive缓冲，由HTTPD串行处理保证无并发覆盖。socket/JSON仍有临时分配，观察实际heap与httpd栈、超时回收；新增测试不证明板上性能或长时稳定。host tests/test_diagnostic_sta.py编译实际diagnostic_sta.c和固定版本cJSON，仅平台/HTTP响应适配，回环模拟SOCKS/HTTP：成功3轮完整SHA以及9个失败子场景（NO AUTH拒绝、CONNECT拒绝、chunked、不合规长header、错误长度、截断、坏下载、上传不匹配、过大回复）全部通过。未用模拟数据冒充板上结果。

构建build-diag36-sta-socks.log成功，无编译器warning/error。快照diag36-sta-socks保存代码/config/bin/ELF和host测试。板上下一步只需打开原AP页面点击上游链路测速，结果BENCH_RESULT由UART自动采集、collect_link_bench.py提取中位数；不再要求抄数字或再次AP测试。Windows直连服务未建立，纯STA和纯直接转发仍待独立验证。

app865952bytes，较diag35增加5472bytes，没有新增固定task/PSRAM payload。已烧录，flash-diag36-sta-socks.log写入hash验证成功、build与快照app完全一致。UART monitor-diag36-sta-socks-live.log持续采集，WSL原18080服务已恢复。新AP页面显示diag36-link-endpoints，但AP结果仍按diag35-ap-local标识，新增上游结果按diag36-sta-socks标识，避免把两种数据混合。板上上游结果待触发，host模拟通过不等同实机通过。

## STA＋SOCKS 板上结果和三段对比

run sta-302056390，三轮下载/上传27.58/25.69、26.78/26.18、26.83/26.51Mbps，中位26.83/26.18。每次8MiB，下载全字节memcmp，三次上传主机SHA256均matches_8m=true。setup_ms下载40.122/10.521/10.402，上传6.930/6.431/8.682，包含在2.4..2.6秒计时中，握手不是这组大传输的主要耗时。没有代理到公网或测VPS出口。

约15.2秒测试覆盖1个完整10秒CPU窗口：Core0/1 idle69.1%/61.2%，wifi30.7%、tcpip23.7%、httpd10.2%、proxy_tcp2.2%、UDP2.5%。只有一窗，不推广为长期平均。内部采样最低94247bytes、最大块51200，RX held0/peak1、TCP重传0、driver TX mem/other累计0。测试中AP handed263→264、STA handed8428→26534，大数据走STA而非AP。

三段单路：AP本地27.69/26.17，STA＋SOCKS端点26.83/26.18，完整代理12.99/12.42Mbps。两端各自均能超过完整路径单路，不能把13Mbps当成AP硬能力或上游SOCKS处理上限。完整路径CPU1 idle41.45%，单路不是持续CPU饱和；而此前4路约20..23Mbps时该核仅余2..7%，多路是另一种约束状态。额外诊断服务占内存、不同采样数/背景连接与时间变化，以及HTTPD端点/非阻塞proxy的实现差异，都限制精确归因。

共享单射频的两段空口、TCP ACK/窗口节奏和非阻塞转发队列等待是单路主要待查方向，已有数据支持这种组合解释，但未分别测出各自占比。两个端点约27、代理约13并不能机械推出物理带宽除二的硬上限：历史4路总吞吐20Mbps以上就是反例，端点应用吞吐也不是纯空口容量。更合理后续是观察代理select等待分布、待发队列和两侧PCB窗口/ACK，再按单路与四路分别判断；函数级CPU耗时用于多路满核路径，不能将单路CPU闲置误诊为需要加CPU频率。

保持板上手动触发的diag36，未追加自动测试或烧录；main与快照恢复一致，build-diag36-match-board.log重建成功，app SHA256再次与已烧录7a51523a992ec9f7ec352d4b33d5bcd2fc3d99368ab95ff46227ff28949cfeba一致。诊断测试已经提供有效数据，本轮不要求重复测速。纯STA直连、不经SOCKS的TCP relay、PHY协商速率/MAC重试/空口占用仍未独立测量，不能声称所有瓶颈已经查明。
