# diag35：先分离 AP 本地端点和完整代理路径

这是临时诊断固件，不宣称性能优化。转发相关packet_rewrite、tcp_poc、socks_relay、flow_table及UDP代码与perf29相同，窗口/任务绑定/Wi-Fi仍相同。主代码变更为app_main启动诊断模块、cpu_profile加入长期httpd任务统计。新增CONFIG_PROXY_LINK_BENCH，Kconfig和sdkconfig.defaults默认n；本次本机sdkconfig显式y。结束后关闭并重建/烧录以移除额外服务及任务。

使用本机ESP-IDF6.1官方esp_http_server，config.if_name绑定AP netif；open_fn检查socket本地地址为AP IP且peer属于AP子网。IPv4 hook现有本地子网放行在严格TCP parse之前，因此Android→192.168.4.1:18081不经透明改写、SOCKS或STA。HTTP server固定一个Core1、优先级5、8192bytes stack任务，不为连接创建task。PSRAM静态生命周期存储24KiB：16KiB重复0..255 pattern，8KiB接收缓冲；不分配8MiB响应，按16KiB chunks流送。上传严格8MiB并按每次短recv的绝对偏移memcmp验证完整内容，浏览器也逐字节验证下载。上传对比消耗ESP CPU，不能称纯射频线速。

/bench复用已有测速算法，每方向8MiB、3轮，仅开放单路；HTTP chunk framing与原主机Content-Length不同，存在少量协议/实现差异，结果解释保留这一限制。/results限制4KiB、JSON嵌套12层、单路、最多3轮、数值有限且正数，complete必须3轮。用cJSON生成escaped JSON并只在测速间隔写UART BENCH_RESULT，响应saved表示已提交串口，不代表持久化Flash。tools/collect_link_bench.py从持续采集的日志提取JSONL并重新计算中位数，无需WSL直连AP子网或用户抄数字。日志捕获必须保持运行。

本机6.1迁移指引确认内置json已移除，使用官方registry espressif/cjson 1.7.19固定版本，main/idf_component.yml与dependencies.lock保存依赖及hash。组件requirements按IDF要求不能依赖尚未装载的Kconfig，所以esp_http_server为固定PRIV_REQUIRES，HTML嵌入及运行入口仍由配置控制；关闭诊断时不启动httpd、不分配测试缓冲。没有修改全局ESP-IDF。

构建build-diag35-ap-local.log成功，未见编译器warning/error；app860480bytes，比perf29多53040bytes，额外内部RAM需看实际启动heap，不能仅将stack等同全部开销。串口结果、内容验证和负载/超时恢复待板上测试；编译不证明链路线速或长期稳定。

测试先Android连接AP打开http://192.168.4.1:18081/bench点单路，再通过页面链接到http://192.168.1.10:18080/bench点单路完整代理路径。两组同一诊断固件、同一手机、停止背景业务；不要拿AP单路对比四路历史总吞吐。额外httpd任务在完整代理测试时空闲但仍占内存，两个路径均承受同样诊断布局。

本机到192.168.1.2:18080尝试直接HTTP连接返回Connection refused，不能直接把现有WSL192.168.1.10当成ESP STA可达服务器。纯STA端点/不经SOCKS的直接转发尚未测试，不自动添加Windowsportproxy、防火墙或改路由。后续解决上游可直连测试服务后再测这一段；经SOCKS的STA测试只能标为STA+SOCKS。

结果解释：AP本地明显高于完整代理说明AP端点有余量，差值包含STA/代理/复制和双段空口成本；二者接近只说明此测试下AP端点已接近完整路径，仍需CPU、驱动及独立STA对照才能判断硬件上限。不会用两组数据直接宣称某一个函数是瓶颈。

烧录完成，flash-diag35-ap-local.log写入hash校验成功，build bin与候选快照SHA256完全一致。UART monitor-diag35-ap-local-live.log持续记录；完整代理页面label diag35-proxy-reference，结果bench-diag35-proxy-results.jsonl单独保存。AP本地结果用PATH=/usr/bin:/bin /usr/bin/python3 tools/collect_link_bench.py monitor-diag35-ap-local-live.log --output bench-diag35-ap-results.jsonl提取。

板上空闲日志确认httpd task Core1、CPU0.0%，内部空闲103579bytes、最大块59392bytes、PSRAM6965632bytes。对比无诊断perf29空闲114623bytes，内部约多11044bytes（10.8KiB）；PSRAM下降25276bytes，包含24KiB测试存储及附带分配。WSL连接ESP STA192.168.1.103:18081返回Connection refused，与AP绑定限制一致；真实AP页面及收发内容待Android验证。启动持续统计无Guru/assert/watchdog，不能由此证明负载稳定。

## 两组单路结果

AP本地run mut0bva0-29vh8lw1he，3轮27.16/26.17、27.69/26.02、28.58/26.76Mbps，中位27.69/26.17。完整代理run mut0d7vg-q1ruzlniucb，3轮12.99/12.42、12.89/12.37、14.40/14.32，中位12.99/12.42。两组内容校验通过，单路对比显示AP本地端点明显有余量；不能直接归因全部差值为复制、SOCKS或射频。也不把单路13Mbps与历史四路20Mbps当成固件回退。

完整代理31秒区间含2个完整CPU窗口，Core0/1 idle64.4%/41.45%，wifi35.35%、tcpip30.5%、proxy27.1%、httpd0.0%；单路不是CPU满载。AP测试仅15.4秒、10秒采样边界未完全覆盖，没有完整CPU窗口，两个重叠窗口混有测试前后空闲，不据此定量比较CPU效率。两组结束后累计driver TX mem仍0、RTO/fast0、RX held0/peak1、PCB failures0；各组内部采样约96.1/99.6KB、最大块47104bytes。

AP端点约27Mbps、完整路径约13Mbps接近二倍差异，但不能机械解释为共享空口恰好减半；还需上游端点测试。同一固件AP+STA一直开启，STA背景管理流量仍存在。下一步diag36通过现有SOCKS测设备STA端点，不改变上游网络；严格称STA+SOCKS而非纯STA。
