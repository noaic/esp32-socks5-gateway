# web44：资源分母、配置分组与网段冲突提示

2026-10-05。已编译、完整内联 JS 语法检查和原浏览器 transport 回归通过；尚未烧录，板上仍为已验证的 web43。固件 883744 字节，比 web43 增加 2080 字节。未增加任务，未修改 NVS schema、转发路径、无线参数、客户端限额或密码。

## 修改文件

- `main/web_management.c`：status 增加内部可分配堆和 PSRAM 堆总量（本机 IDF `heap_caps_get_total_size`），AP IP/掩码、STA 掩码、网段重叠状态，以及来自实际宏/设置的客户端、代理流、TCP PCB、HTTP 栈容量。HTTP 栈容量与任务创建复用同一常量。
- `main/management.html`：已占用/总量/剩余；TCP PCB 当前/容量、峰值/失败；HTTP 历史最大使用/栈总量；配置分为上游网络与代理、本地 AP 与客户端网络、管理安全。
- `docs/web43.md` 与 `bench-web43-analysis.json`：记录实际并用验收。

## 指标含义

可分配堆总量不是 S3 全部物理 SRAM。已占用按总量减剩余估算，包含分配器开销；各内存读取是依次采样而非全系统原子快照。PSRAM 物理容量仍是 8 MB。HTTP 栈显示总量减历史最低剩余，是本次启动历史最大使用，不是当前值。错误/拒绝/重传是累计事件数，没有有意义的“容量”分母。

页面 TCP/UDP 流使用量为当前关联客户端 registry 快照之和，容量来自全局代理限制；不是 TCP PCB 数。TCP PCB 包括客户端、上游、UDP 控制以及关闭/TIME_WAIT。

## AP 网段结论

当前工程未启用 NAT。透明代理和 STA sockets 共用 lwIP 路由：本机 IDF 的 `ip4_route()` 遍历活动 netif，返回首个目的网段匹配的接口。同网段会产生出口歧义；客户端访问其本地子网中的上游 IP 还会先 ARP，不经网关。

本版仅在状态页报警，未实现自动修复或自定义网段。当前 UPSTREAM_WIFI 192.168.1.0/24 与 AP 192.168.4.0/24 不重叠，继续使用不需要改网段。

建议下一版增加 `ap_ipv4`，掩码先固定 `/24`，限制 RFC1918 私有地址且主机部分有效；不把“更少见的默认网段”当作避免冲突的保证。改造需要：

1. NVS 新 schema 和旧记录迁移，active/previous 两份都保留；确认/回滚涵盖 AP 地址。
2. 统一 AP IP 的来源：app_main、TCP 本地改写目标/测试地址检查、管理链接/Origin、诊断和日志。UDP 现有 netif 地址获取继续保留。删除地址校验中对旧 AP /24 的硬编码排除。
3. DHCP 停止、应用 IP/gateway、配置租约后再启动（本机 API）；启动前应用，避免运行中修改导致旧租约/流表混用。
4. 保存时拒绝与当前 STA 掩码所代表网段重叠的候选、拒绝 SOCKS5/DNS 落在 AP 子网。重叠比较必须涵盖不同掩码，不限于 /24 与 /24。
5. STA 新取址时再次检查，因为保存时可能尚无地址或切换了上游网络；保留 AP 管理，明确显示冲突状态。暂不实现自动换段，避免手机缓存租约及管理 URL 突然失效。
6. 更改地址前明确新管理入口；新址登录并确认，否则恢复旧地址与配置。测试覆盖旧记录迁移、非默认地址 TCP/UDP、双客户端、重叠/包含网段、无 STA 和回滚。

资源增加主要是两份地址字符串/配置和少量校验，远低于 Wi-Fi/socket 缓冲；复杂度集中在地址一致性、迁移与租约恢复，不是性能开销。

官方参考：https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_netif_programming.html 。具体 API 和路由行为以本机 IDF v6.1 源码为准。

## 验证命令

`source /home/user/.espressif/tools/activate_idf_v6.1.sh`，然后 `idf.py build`。日志 `build-web44.log`。如有 Node：`node tests/management_transport_harness.js`；本次在工具 V8 中执行同一测试，并检查两份 HTML 完整 JS 语法。未重复运行 web42 的 52 项全量回归。尚无 web44 板上验收，不能拿 web43 的吞吐作为新版实测。
