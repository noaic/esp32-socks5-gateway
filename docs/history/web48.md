# web48：经 SOCKS5 的互联网状态检查

2026-10-05。按最新决定实现手动检查与低频自动检查；NTP、BLE、OTA、自动重启均不增加。自动检查首次默认开启，可在管理页关闭并保存，重启保持设置。

## 协议与执行

复用 gateway_control 的一个 6144 字节固定任务；HTTP handler 只排队，不阻塞管理页面等待公网响应。互联网检查与重连/重建/重启/原有诊断串行执行，不改变代理任务优先级、无线参数或流表容量。正在检查时其他维护请求返回繁忙，最长约 13 秒。

1. 上游 TCP 端口连接，期限 3 秒。
2. SOCKS5 NO AUTH，期限 2 秒。
3. 临时 UDP ASSOCIATE，经当前 DHCP DNS 地址查询检测域名 A 记录；期限 4 秒包含关联握手与查询。验证 SOCKS 包、源 IP/端口、DNS ID/问题/RCODE，解析最多 32 条答案及 8 层 CNAME；拒绝无关/私有/保留地址及格式错误。
4. 关闭 DNS 两个 socket，另建 SOCKS5 IPv4 CONNECT 到解析地址的 80 端口，用 HTTP/1.0 + Host 获取小响应；总期限 4 秒包含连接、握手及 HTTP。固定 1024 字节接收缓冲，验证状态和正文，不跟随重定向，不接受 chunked。

峰值两个临时 socket，没有逐连接任务或 TLS 堆。DNS 和 HTTP 均经过配置 SOCKS5；没有 ESP32 直连公网兜底，Xray 的 direct/proxy 路由仍由上游决定。此检查独立于客户端 UDP 流表，不消耗其 32 个业务槽位，但临时 socket/PCB 仍占共享 lwIP 资源。DNS 检查结束再做 HTTP，临时 TCP 会有正常 TIME_WAIT。

管理页允许选择两个固定检测目标，而非任意 URL：

- 默认 Microsoft：`http://www.msftconnecttest.com/connecttest.txt`，HTTP 200 且正文必须完全等于 `Microsoft Connect Test`。
- Android：`http://connectivitycheck.gstatic.com/generate_204`，HTTP 204 且接收内容无正文。

来源：[Microsoft NCSI 官方说明](https://techcommunity.microsoft.com/blog/networkingblog/ncsi-change-notification/3866600)、[Android NetworkMonitor 源码](https://android.googlesource.com/platform/frameworks/base/+/b98766ec7785296b642b0a809c1be6107d1f9bcf/services/core/java/com/android/server/connectivity/NetworkMonitor.java)。这是小型 HTTP 可达性检查，不是 HTTPS 身份认证、全网站保证、测速或 QUIC 检查；一个目标失败不能直接认定整个互联网不可用。可手动切换另一个目标，但不会自动额外探测备用目标。

## 调度、状态与灯

自动模式：AP+STA 就绪后约 2 秒首次检查；成功后 5 分钟；失败后 30/60/120/240/300 秒退避。网络配置处于 180 秒试运行时自动暂停，确认后恢复；用户仍可主动诊断。维护任务繁忙时等待。检查失败不重启、不关闭现有业务流、不改变网络配置。

状态与结果有效期使用单调时间，不依赖 NTP。AP/STA 变化、网段冲突、代理重建、检查设置更改会使旧结果失效；结束时核对网络代次，旧网络结果不能重新点亮绿灯。管理页显示分阶段错误/耗时、目标 IPv4/HTTP 状态、距完成/成功时间、连续失败及下次检查。

灯开关仍可持久关闭。蓝：上游未配置；红：STA 未就绪；紫：AP+STA 就绪但结果未知/检查中/首次失败/过期；绿：当前网络最近检查成功，10 分钟内有效；黄：网段冲突或当前目标连续至少两次检查失败。关闭自动后可手动点亮绿灯，过期后转紫。

检查开关与目标单独保存至 NVS `healthcfg/prefs`（4 字节版本记录），不改变网络配置 v2 或管理口令，不要求重启。重复保存不写 Flash；提交失败保留旧值；与 worker 的设置提交及排队互斥。网络试运行回滚不会回滚此独立偏好，和灯开关一样。

## 文件与验证

- gateway_probe.c/h：DNS A/CNAME 和经 SOCKS CONNECT 的 HTTP 检查；保留旧三阶段手动诊断接口。
- gateway_state.c/h：检查状态、代次、有效期、退避和紫色灯策略。
- gateway_control.c/h：复用 worker、自动调度与 NVS 偏好；增加明确的 Web 重启 UART 标记。
- gateway_settings.c：pending 标志改为 C11 atomic，供 worker 安全读取；配置记录布局未改。
- web_management.c、management.html：受鉴权/同源保护的检查操作与设置，状态展示；保留旧浏览器超时兼容。
- tests/test_gateway_internet.py：生产 C 对本机假 SOCKS/DNS/HTTP 服务的协议测试；state/control 测试补覆盖过期、断网旧结果、退避、调度及设置提交失败/持久性。

`python3 -m unittest discover -s tests -v`：71 项通过，32.892 秒，test-web48-all.log。两种目标成功、分段握手、CNAME 乱序、私有/无关/循环/截断 DNS、HTTP 重定向、正文/长度错误、过短/超大/错误 header、SOCKS 拒绝和严格超时均覆盖。完整管理页 JavaScript 语法检查通过。

`idf.py build` 无 warning/error，build-web48.log。bin 918752 字节，比 web47 增加 10736 字节；DRAM data 22951、bss 27672 字节，bss 增加 96 字节。未新增任务或任务栈；socket/PCB 临时开销与栈峰值另看板上记录，不把静态 size 当完整运行资源。

WSL 运行生产检查函数，通过现有 192.168.1.2:7897 上游：Microsoft DNS/HTTP 成功 472/503 毫秒，HTTP 200；Android 470/471 毫秒，HTTP 204。此结果不是 ESP32 板上性能或手机验收。板上日志、任务栈和用户验收随后记录。

回退可使用 build-snapshots/web47-daily-diagnostics 的 bin/elf 或源快照重新构建；不擦 NVS，网络 v2 与口令保持兼容。旧版忽略新增 healthcfg 偏好。

## 板上启动记录

flash-web48.log 已完成写入 hash 校验，未擦除 NVS。09:47 启动确认 AP192.168.10.1、max_clients=2、pending=0；原管理口令与网络配置沿用。STA 192.168.1.103 就绪时内部 free94199 字节、PSRAM free6985860 字节；web47 启动采样为94339/6985860，当前内部少140字节，启动非原子样本，不视作负载峰值。维护 startup_delta7468、HTTP8664字节，固定栈大小保持6144。

09:47:21 板上第一次自动检查 target=0、auto=1、accepted=1、code=0、HTTP=200。Web 手动、设置关闭及重启持久性、实际任务栈峰值仍待手机验收。未重跑吞吐，不能据此承诺零吞吐影响。串口持续采集 monitor-web48-live.log；源代码、构建配置、bin/elf及测试/烧录日志保存在 build-snapshots/web48-internet-health。

补充验证：控制任务测试新增“未确认网络配置暂停自动检查，确认后恢复”的场景，单项重跑通过；管理请求 helper 在 V8 中实测缺少 AbortSignal.timeout、fetch/XHR、鉴权错误与请求超时兼容均通过。Node 本机未安装，实际 JS 验证使用工具 V8 执行，没有据命令不存在宣称通过。

09:48:43 与09:49:00 串口又收到两次手动检查（auto=0），均 accepted=1、code=0、HTTP=200。手动执行路径已在真实板上成功；手机页面显示、肉眼灯色及偏好重启持久性仍不以 UART 成功替代。

用户验收：Android 手动检查后回复“绿色，检查通过”，确认页面结果和实际 WS2812 绿灯一致。未提供维护任务栈数值，故不推算栈峰值；检查偏好关闭/重启持久性仍未做板上验收，已有宿主提交失败、重复写入及重启加载测试。
