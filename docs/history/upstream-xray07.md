# 上游 Xray 26.1.18：接入 DNS 分流

用户确认当前实际运行的是 Xray 26.1.18，第一条 outbound 为 `tag: proxy`、VLESS/VISION/REALITY，其后已有 `protocol: freedom, tag: direct`。保留这两条及既有国内/私有IP和国内域名路由。

用户最初回答“只有proxy”，随后核对并纠正direct已存在。因此撤回缺失direct的故障诊断，不把它用作当前APP延迟/重置的证据。源码关于不存在tag会关闭连接的行为仍成立，但不适用于已更正的配置。

## 明文 DNS 请求进入已配置的内置 DNS

这些都是配置片段，不能覆盖包含现有 proxy 服务器参数的整份配置。

1. 在 outbounds 末尾再加 `examples/xray-esp32-dns-outbound-compat.json` 的对象：

```json
{"tag":"esp32-dns","protocol":"dns","settings":{"nonIPQuery":"skip"}}
```

2. 在 routing.rules 最前面按下面顺序加入两条。原来的 private/cn IP 和 cn domain 规则放在它们之后：

```json
[
  {
    "type":"field",
    "source":["192.168.1.103/32"],
    "network":"tcp,udp",
    "port":"53",
    "outboundTag":"esp32-dns"
  },
  {
    "type":"field",
    "inboundTag":["esp32-dns-upstream"],
    "outboundTag":"proxy"
  }
]
```

第一条只接管来自 ESP32 STA 的明文 53 端口请求；第二条把内置 DNS 的非 local 查询明确交给 proxy。ESP32 STA 地址来自 DHCP，需保持租约/更新该源地址规则。

3. 用 `examples/xray-dns-settings.json` 的内容更新现有 dns 对象：在原配置添加 `"tag":"esp32-dns-upstream"`，把 Cloudflare 地址改成 `"https://1.1.1.1/dns-query"`。其他服务器、缓存、IPv4 和 fallback 设置延续用户配置。

这里使用 Cloudflare 支持的 IP DoH endpoint，避免必须先解析 cloudflare-dns.com；TAG 路由明确走现有 proxy。阿里 `https+local://` 继续从 Windows 本机直接发起，不经过该路由。Xray 官方文档说明 IP 形式 DoH 与域名形式的解析回环注意事项：[内置 DNS](https://xtls.github.io/config/dns.html)。

合并后的 outbounds 顺序应为：现有完整 proxy 对象、direct、esp32-dns。保持入站 SOCKS UDP=true、IPv4 advertised IP=192.168.1.2 和端口7897。此次不添加 sniffing 或改变已有分流规则。

v26.1.18 使用 compat 片段。第六轮提供的 `xray-esp32-dns-outbound-current.json` 是已核对 v26.9.30 的 rules 写法，当前版本不要使用它。新版本目前仍兼容 nonIPQuery，但会提示弃用。

这会让 A/AAAA 进入内置 DNS 缓存和域名分流；其他记录用 nonIPQuery=skip 放行给原目标 DNS，不保证这些类型也享受内置分流。共享全局 DNS 配置的其他入站解析也会使用新的 DoH endpoint/TAG。客户端加密 DNS 不匹配明文端口53规则。

## 最短验证

在 Windows 上先验证合并后的配置再启动（将路径替换为实际配置文件）：

```cmd
xray.exe run -test -config config.json
```

保留现有启动方式重启内核。在连接 ESP32 AP 的 Windows 可执行：

```cmd
nslookup example.com
```

显示 Server=one.one.one.one 仍然正常：客户端的目标地址仍为1.1.1.1，透明 DNS 出站不会把 DHCP DNS 换成 Xray 地址。是否进入内置 DNS 应核对 Xray 的 DNS 日志/所选 DoH，不靠 nslookup 的 Server 名判断。

Android 无需运行脚本，刷新 B 站首页、打开网页、播放并拖动进度即可。按同一操作比较之前的卡顿；TCP 首字节/UDP DNS RTT 分别可在新固件定时日志看到。

本机只完成 JSON 语法和当前版本源码核对。Windows Xray 配置需用户合并；尚未在该二进制上运行以上 `run -test`，也未声称已启用上游 DNS 分流。
