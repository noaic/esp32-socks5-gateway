# web45：可配置 192.168.X.0/24

## 行为和文件

用户只修改第三段 X（整数 0–255），AP 网关固定 192.168.X.1，掩码固定 /24。未增加任意网关、DHCP 地址池或掩码配置。保存仍为约 2 秒后重启，所有连接中断；回复及页面确认框显示新 URL，重连后在 180 秒内确认，否则回滚旧网段和整份配置。

- `main/gateway_settings.h/.c`：持久化 ap_subnet，默认 4，v1 -> v2 迁移。原字段尾部 padding 1 字节由 ap_subnet 使用，结构296/record600字节均不变，static_assert 固定 ABI。迁移不读取旧 padding，active/previous 都补 4，保留两份口令、上游、trial 状态；提交失败返回错误不宣称成功。
- `main/settings_validation.c`：统一 IP 格式化与网段重叠检查；SOCKS5/DNS 禁止落在当前候选 AP /24，取消旧 .4 的固定排除。重叠检查覆盖相同、较宽及较窄上游掩码。
- `main/management_request.c`：ap_subnet 只接受 JSON 整数 0–255，字符串/布尔/负数/小数拒绝。
- `main/app_main.c`：启动前停止 DHCP、应用配置 AP IP/gateway、配置 DNS、再启动 DHCP。STA GOT_IP 用实际 IP/掩码复核；冲突时 TCP/UDP readiness=false，AP 与管理服务保留。
- `main/tcp_poc.c`：本地 IPv4 改写目的和测试 IP 检查采用配置网关，mapping 日志不再打印固定 .4.1。UDP 已取 AP netif IP，无需另加地址源。
- `main/web_management.c`：配置读取增加 X，保存检查同一 STA SSID 当前地址重叠，更换 SSID 的未来地址由 GOT_IP 复核；回复含新管理 URL；status/Origin/接口绑定均使用当前地址。
- `main/management.html`：第三段输入，强制重启及新/旧管理入口提示。复用 web44 的资源分母和配置分组。
- `main/diagnostic_bench.c`：诊断 URL 日志使用实际 AP 地址。
- `tests/test_gateway_settings.py`：扩展生产 C 校验、边界、地址重叠、动态 SOCKS/DNS 排除、v1 padding 迁移、迁移提交失败、trial/确认/网段回滚测试。

静态 NVS record 和 boot_settings 大小未变，未增加任务、Wi-Fi 缓冲或流容量。web45 固件 884864 字节，web43 881664 字节，增量 3200 字节（包含 web44 UI）。

## 验证

`idf.py build` 成功，`build-web45.log`。浏览器 transport 回归与完整两页 JS 语法检查通过。扩展的 settings/JSON/NVS 两项宿主测试通过。全量52项回归包含本机socket，须在允许socket的环境运行：`python3 -m unittest discover -s tests -v`，日志 `test-web45-all.log`。

板上升级与非默认网段验收另行追加，不能用宿主测试替代。

## 使用与边界

示例 X=10：先在旧管理页保存，Wi-Fi 会断开；重新连接后打开 http://192.168.10.1/，使用原口令登录并确认，再验证 DNS/网页/测速。两台设备都必须重获租约。若180秒内未确认，设备重启回旧网段，此时需再次重连并打开旧地址。

配置没有自动换段。冲突检测会提示用户；如果上游是包含所有192.168地址的/16，X的任何值都无法避让，当前限定范围下应修改上游网段。选124会因现有上游SOCKS地址192.168.1.2落入候选AP网段被拒绝。

既有缓存 DHCP 租约未立即登记问题尚未作为本次功能修复；若重启后没联网，手动关闭/打开该 Wi-Fi 重新获得地址。RST不清口令。本版记录version=2，不应直接烧录只接受version=1的旧管理固件来回退，否则旧代码会重置配置；固件回退需要配套记录迁移。

旧 PoC 工具 `tools/test_transparent_echo.py` / PowerShell 默认 .4，仅在默认网段下用；本版非默认地址验收使用实际客户端与管理页面，不将旧工具默认值当作固件限制。

## 烧录与当前板上状态

04:30 已烧录，`flash-web45.log` 写入 hash 校验成功。UART `monitor-web45-live.log` 确认 AP 为192.168.4.1、max_clients=2、管理服务pending=0，上游192.168.1.103/24，AP+STA ready。捕获器打开时复位过一次，首次 flash 后执行的迁移日志可能在采集开始前；不能以日志缺少迁移行判断是否迁移失败。旧口令和非默认网段仍待用户操作验收。

测速服务保留原入口 http://192.168.1.10:18080/bench，标签 web45-ap-subnet，报告自动保存到 bench-web45-results.jsonl。尚未要求本轮测速。

全量52项回归通过，耗时23.864秒；第一次在沙箱内执行因socket权限失败，随后在允许本机socket的执行环境重跑通过。

## 非默认网段验收（04:32–04:36）

用户确认“原口令正常，新地址正常，网页正常”。串口对应软件重启后 AP 192.168.10.1/24、DHCP 客户端192.168.10.2 tracked=1，STA仍192.168.1.103/24，ready=yes。管理接口在新地址响应成功，HTTP剩余栈3652/6144字节。记录中 TCP双向字节增长、DNS UDP回复/回包增长（到04:35:23 DNS in/sent/reply=101/73/33）。新网段运行超过180秒未记录自动回滚；接口未单独打印confirm成功，不能把不存在的日志当作持久化读取证据。尚未另行复位验证已确认网段的持久性，双客户端新址、真实上游重叠和超时回滚板测也未覆盖。

此外自动测速出现一次单路run `muua4u82-3k93wff73io`：首轮16.67/14.73 Mbps，8MiB上传校验通过，随后Failed to fetch，没有三轮完成报告。服务端第二个GET也发送8MiB，但没有对应第二轮上传请求；同期PCB分配失败0、两侧TCP重传0、驱动发送错误0、改写drop0。保留bench-web45-results.jsonl与监控记录，失败原因未确认，不能将这次作为完整性能验收或归因于改网段。功能验收与性能测试分别记录。

## 后续完整测速验收（04:37–04:39）

用户确认单路、多路均正常，自动报告确认两组各三轮完整完成：

- 单路 `muua83g9-ci8aynyys65`：下载/上传中位数 15.87/15.16 Mbps。
- 四路 `muua8twl-cnnxlgl53on`：下载/上传中位数 24.25/20.75 Mbps。

两组运行均为新 AP 网段 X=10，一台关联客户端。旧的 Failed to fetch run 保留，不再用它代表后续测速结果，也不宣称其原因已定位。详见 bench-web45-analysis.json。

04:39:22 累计快照：TCP PCB峰值134/256，分配失败0；AP RX/STA RX rejected=0，Wi-Fi发送完成data_fail=0。短暂driver TX NO_MEM累计AP60/STA41，客户端RTO/fast各2，上游各0；并非所有错误计数为零。已完成本轮功能与性能验收，尚未测试新网段下两客户端并用、实际重叠场景、额外重启后的持久性及长期运行。
