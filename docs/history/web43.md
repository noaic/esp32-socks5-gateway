# web43：修复管理页浏览器兼容性

2026-10-05。基于 web42 首次口令流程，不修改网络配置、Wi-Fi、转发数据路径、连接限额或口令存储。已编译、前端 transport 回归通过并烧录，写入 hash 校验成功。

## 已定位的问题

终端 B 顶部明确提示 `AbortSignal.timeout is not a function`。此前页面直接依赖该较新的浏览器 API，导致 config/status 请求在构建请求参数时就失败。HTML 可显示并不代表接口可用，HTTP 任务低占用也不能证明 B 已成功轮询。

管理页及首次设置页改用普通 `setTimeout` 与 `AbortController`，请求全过程上限 8 秒，完成后清理计时器。缺少 AbortController 时使用 XMLHttpRequest 的原生超时兜底。保留原有 Basic 凭据、同来源策略、JSON 和修改接口请求头。不扩大 HTTP 连接数或新增任务。

服务端每个启动最多记录前两个客户端首次成功发送 status API 响应的 IP 与任务剩余栈，便于确认接口是否实际执行，不记录认证头、口令或配置请求体。

修改 `main/management.html`、`main/management_setup.html`、`main/web_management.c`；新增 `tests/management_transport_harness.js`。页面状态版本为 `web43-browser-compat`。

## 测试

- `idf.py build` 成功，日志 `build-web43.log`。
- 执行真实 HTML 中的 requestJSON helper，模拟缺少 AbortSignal.timeout：fetch GET/POST、Basic 凭据传递、修改请求头、错误反馈、8 秒取消、XHR 兜底与超时通过。
- 两个 HTML 页面共享的 helper 内容一致。JS 测试文件可在有 Node 的环境执行 `node tests/management_transport_harness.js`；本机没有 Node，本次通过工具提供的 V8 执行同一测试。
- 原有 52 项本机全量回归在 web42 已通过；本次改变 HTTP 页面 transport 和有界日志，没有声称重新执行 52 项。
- 终端 B 已重新打开页面，用户确认运行状态和配置均正常显示；串口记录到 status API 成功响应。管理页与转发并用仍待下面的独立验证。

## 下载异常独立记录

web42 期间第二台终端关联后，A 两个 4 路 run 没有完成任何一轮，服务端发送超时，客户端侧 RTO/fast retransmit 明显增加。B 当时因为 API 兼容性错误没有发起正常的管理轮询，因此不能把这次转发失败归因于管理轮询 CPU 开销。

旧报告末尾还收到两次后来完成的测试：单路中位数 14.22/13.98 Mbps、4 路 24.22/20.41 Mbps。完成时两台仍关联，没有记录到主动断开事件，也没有修改固件或无线参数。这说明有间歇性恢复，但当时用户操作和页面状态未完整确认，不能作为管理页负载 A/B 或据此宣称已解决。

所有有效/失败报告保留在 `bench-web42-results.jsonl`；旧监控 `monitor-web42-reboot.log`。先验证 B 的接口，再单独诊断双客户端的数据重传问题。web42 重启后缓存租约未立即登记的问题也仍存在，当前需手动重连 AP；未取消 MAC/token 隔离来绕过。

## 当前采集

烧录日志 `flash-web43.log`，持续串口 `monitor-web43-live.log`。已保存旧日志，旧标签测速服务已停止。B 验收后，原地址 `http://192.168.1.10:18080/bench` 已恢复，标签为 `web43-browser-compat`，自动报告保存到 `bench-web43-results.jsonl`。当前等待一次 B 管理页前台、A 四路测速的实际并用验证。

## B 验收（04:08）

用户确认运行状态与配置正常显示。B MAC `02:00:00:00:00:02` 首先接入本次启动的 AP，获得 `192.168.4.2`，所以不能按上次启动 IP 判定 A/B。04:08:35 串口确认 status API 响应成功，HTTP 最小剩余栈采样 3676 字节（栈总 6144 字节）；没有记录口令。启动后 AP+STA ready、管理服务正常。

在这个确认点，A 尚未接入。下一组要求 B 管理页保持前台、A 接入后单独做一次 4 路吞吐，测试真正的并用场景；若失败转入定位，不能把上次没有发出轮询的样本当作已完成此验证。

## 管理与转发并用验收（04:13–04:14）

A 四路三轮完整完成，B 保持管理页且用户确认正常。中位数下载 23.55 Mbps、上传 20.15 Mbps。报告 `bench-web43-results.jsonl`，采样归纳 `bench-web43-analysis.json`。

04:13:00–04:14:38 采样窗口内部剩余最小 94271 字节、最大连续块最小 49152 字节、PSRAM 剩余最小 6857552 字节；TCP PCB 峰值 101/256，分配失败 0，HTTP CPU 采样最大 0.2%（100%=一核）。TCP 客户端 RTO 0、fast 增加 1，上游 RTO/fast 0。驱动短暂 TX NO_MEM AP 30、STA 16，以及 UDP 配额拒绝仍有记录，不能声称零错误或已完成长期稳定性验证。

这次正常通过真实管理页并用场景；它不解释此前间歇性失效，也不是严格的管理服务开启/关闭 A/B 性能实验。
