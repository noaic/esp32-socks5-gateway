# web46：统一状态、事件、维护诊断及 GPIO48 WS2812

按既定顺序实现：先状态/事件，再 Web 维护，手动诊断，最后 LED。网络配置仍为 v2，旧口令和 AP 网段不变；LED 开关单独保存在 NVS namespace indicators/key enabled，默认开启。不加入 BLE、OTA、物理恢复或串口控制台。

## 文件与架构

新增 gateway_state.h/.c：64条静态事件环形队列（约2048字节），各任务的短临界区快照；相邻相同kind/detail、间隔10秒内合并，保留首次/最近启动时间、count、sequence。无堆分配、socket、Flash操作或格式化日志。没有每流成功事件，TCP首响应/CONNECT时间和UDP/DNS成功发送时间用独立状态快照。错误记录不含访问目标、密码或认证头。

新增 gateway_control.h/.c：一个固定gateway_control任务，优先级2、栈6144、不绑定核心，任务队列深度1；任务未完成拒绝新任务。重连STA沿用现有断线事件定时重连，保留AP；重建通知两个代理worker各自关闭流，维护线程不操作它们的socket。重启保留NVS，试运行配置时拒绝Web重启，避免误回滚。维护运行中拒绝保存新网络配置。

新增 gateway_probe.h/.c：非阻塞socket配合select和绝对阶段期限（端口3s、NO AUTH2s、UDP DNS4s）；一个诊断至多2个socket，始终关闭，UDP控制TCP保持到诊断结束。SOCKS5 UDP请求configured DNS的example.com/A，验证来源、FRAG=0、事务ID、QR/RCODE/原问题/答案非空。未直接查询DNS、未探测VPS、未发起额外HTTP请求。阶段失败则后续标未执行；只有通过UDP回包才标DNS成功。超时等待每200ms给LED更新一次机会，避免灯开关等完整诊断结束。

官方espressif/led_strip固定3.0.3，用RMT GPIO48、单颗WS2812 GRB，10MHz、无DMA，亮度各通道不超过8。只在颜色改变时发送，LED操作仅在固定维护worker及启动初始化中执行，Web不调用driver。关闭设置立即提交NVS，正常约200ms内发送黑色，诊断时也更新。重复开关不写NVS，提交失败不改变运行开关。启动关闭状态仍发黑色清除暖重启可能遗留的亮色。RMT驱动失败不阻止转发，Web显示错误。

颜色：蓝=上游未配置；红=配置了上游但未就绪；绿=AP+STA就绪/无冲突；黄=网段冲突或最近30秒内TCP上游连续失败至少3次且未有新响应；关闭最高优先级。绿不是Internet可达承诺，黄色也不是已定位全局上游故障。

main/app_main.c 接入AP/STA/客户端事件与控制worker。gateway_settings.c 仅成功提交后发配置stage/confirm/rollback/首次口令事件。socks_relay.c 接入CONNECT成功、首响应及上游错误；客户端取消/正常idle不作为上游失败。udp_relay.h/.c 可选association_error回调，原worker行为不变。udp_gateway.c 接入错误回调及输出成功时间；这里表示IP输出调用成功，不代表客户端应用已经收到。

main/web_management.c 增加认证GET /api/events（最新32条，静态1024字节scratch，避免大栈），认证POST /api/action（reconnect/relay_reset/reboot/diagnose/led）。沿用同来源/修改头/JSON校验，没有未认证维护入口。status包含各最近时间、错误、任务/诊断/LED结果与维护栈容量，仍不自动探测。max URI handlers由8改10，HTTP连接数仍2、优先级3、栈6144。事件cJSON和响应只在请求时暂用堆。

main/management.html 增加状态、任务、诊断、事件区，前台10秒顺序拉status/events，不并行占两条HTTP连接；后台暂停。无SSE/WebSocket。重启明确提示重连同一管理地址，恢复默认网络配置仍保留口令。

main/CMakeLists.txt、idf_component.yml、dependencies.lock 增加模块和官方LED组件。未修改全局IDF安装或lwIP patch。

## 验证与资源

编译成功，build-web46.log；60项宿主回归通过（28.180秒），test-web46-all.log。新增test_gateway_state.py（转换/环容量/重复合并/并发计数/灯色策略）、test_gateway_probe.py（实际C、碎片握手/DNS往返、NOAUTH/REP/RCODE拒绝、无关回包过滤及4秒期限）、test_gateway_control.py（队列互斥、维护调用、LED NVS提交失败/重复不写/重启关闭/HTTP不调用driver）。原settings测试增加状态依赖stub。两页完整JS语法及真实HTML transport回归通过，保留旧浏览器XHR兼容。

web45基线884864字节，新版922992字节，增量38128字节（约37.2 KiB）。链接快照DRAM bss +3280字节、DRAM data +468字节、IRAM text约+5004字节，内部heap起点约后移8608字节（包含对齐，不能把GNU size总bss当内部RAM）；新增6144字节任务栈、TCB、1项队列与RMT driver运行分配，实际启动delta取UART采样。未改变PSRAM流缓冲、Wi-Fi buffers、TCP窗口或client容量。增量不是零，最终吞吐/双终端并用待板测。

## 板上验收与边界

烧录、登录/诊断/灯开关/关闭后重启持久性、重连/重建操作与吞吐结果将另行追加。不能把宿主mock验证当板测。事件重启清空，没有墙钟；代码0/负errno/1000+REP/2000+METHOD/3000wire-invalid需要结合字段使用。设置灯不会触发网络配置重启或180秒确认。

组件led_strip_refresh本机源码使用rmt_tx_wait_all_done(...,-1)，驱动异常可能阻塞维护worker；已隔离于HTTP和代理worker，未手改官方组件。诊断网络等待期限不包括异常driver阻塞。无周期主动探测，不因单次诊断失败自动重启/断网。

## 首次烧录（05:16）

flash-web46.log Hash verified。串口 monitor-web46-live.log：AP=192.168.10.1、max_clients=2、pending=0；STA=192.168.1.103/24，AP+STA ready。Control worker 栈6144/priority2，WS2812 GPIO48 ready=1 enabled=1，说明RMT创建及初始化发送完成，实际颜色和开关仍待用户目视确认。

控制模块启动 internal_delta 采样7468字节（任务、队列、driver，可能包含并行Wi-Fi变化）；新增HTTP路由 startup_delta8664字节，旧版8600。加上内部静态布局约8608字节，新增占用约16KiB量级。STA就绪时内部free88331字节、PSRAMfree6985860字节。启动正常，没有该采样点前的panic；负载最低内存、HTTP/维护栈与吞吐待实测。

已要求用户：新管理页登录，运行三阶段诊断，关闭灯并通过Web重启，确认灯开关持久与原口令/新网段保留。重连/重建代理及吞吐随后验证；不能提前宣称已板测通过。

## 用户验收与完整测速（05:19–05:23）

用户确认要求的诊断、关闭灯、Web重启与关闭设置保持测试完成无问题。UART05:19:49软件重启，05:19:51 GPIO48 driver ready=1 enabled=0，管理入口仍192.168.10.1 pending=0，05:19:55 AP+STA ready；新地址确认状态及LED关闭均跨额外重启保留。05:20:20管理status响应成功，HTTP最小剩余栈3620/6144字节。诊断阶段结果在Web返回，UART未独立打印结果，依据用户验收，不虚构串口诊断成功行。

自动报告确认：单路run muubsip3-ums4ukzfq6，三轮完成，中位数15.51/15.31 Mbps；四路run muubt5h4-ktzep6sw8gd，三轮完成，中位数24.20/21.02 Mbps（下载/上传）。与web45最近样本15.87/15.16、24.25/20.75接近，非严格A/B，不能解释为性能提升或保证无任何开销。该组只有一台AP客户端，LED处于关闭状态。

重启后日志采样TCP PCB峰值125/256、分配失败0；内部RAM启动历史最低35576字节（约34.7 KiB），采样末尾free约80KiB，实际内存余量已比web45下降。05:23:04两侧TCP RTO/fast为0，Wi-Fi completion data_fail=0；driver TX NO_MEM累计AP33/STA23，仍保留对应netif_error记录，不能声称零错误。

结果保存 bench-web46-analysis.json、bench-web46-results.jsonl、monitor-web46-live.log，源代码/固件快照build-snapshots/web46-state-maintenance。Web重连STA和重建代理操作没有逐项板测确认，双客户端并用和长期稳定性也未覆盖；本轮不继续要求重复测速。
