# 上游 UDP/443 对照检查

2026-10-03，在 WSL 经 eth0 直接连接 192.168.1.2:7897，绕过 ESP32。
这是上游对照，不是 ESP32 AP 路径验收。WSL 经过 NAT 后的源地址也可能
不同于 ESP32 STA，不能用这里的 DNS 结果证明 ESP32 源地址 DNS 分流生效。

## 实测

- SOCKS5 NO AUTH、UDP ASSOCIATE 成功。
- 1.1.1.1:53 查询 example.com：成功，约484ms。
- 1.1.1.1:53 查询 www.bilibili.com：成功，约482ms。
- 经同一 SOCKS5 对 1.1.1.1:443 发起 QUIC TLS 握手，SNI
  cloudflare-dns.com、ALPN h3、证书验证开启，最大 QUIC payload1200bytes：
  8秒窗口tx6/rx0，超时。
- 板上12e累计UDP443 sent96/reply0；不同目标、不同路径，不视作上述
  单个QUIC探测的同一请求。TCP/DNS仍有成功转发。

## 官方行为与配置修改

核对用户实际版本的源码：
https://github.com/XTLS/Xray-core/blob/v26.1.18/proxy/vless/outbound/outbound.go

当前main也保留相同分支：
https://github.com/XTLS/Xray-core/blob/main/proxy/vless/outbound/outbound.go

VLESS outbound的普通`xtls-rprx-vision`会拒绝UDP443；
`xtls-rprx-vision-udp443`设置allowUDP443，然后移除后缀再发送标准Vision
flow。因此本轮建议只修改Windows Xray的proxy出站中已有flow值：

```json
"flow": "xtls-rprx-vision-udp443"
```

保持现有vnext/users或其他配置结构，不用新文档的完整结构替换旧配置。
保留VPS端现有Vision设置、UUID、REALITY参数、直连路由与DNS配置。
该调整会影响使用这个proxy出站的所有客户端，不只ESP32。UDP443走direct
时不经过该Vision拒绝分支。用户实际配置尚未直接读取；超时本身还可能
包含其他路由、出口或目标限制，需修改后用相同探测确认。

官方文档：https://xtls.github.io/config/outbounds/vless.html#flow-string

## 工具

`tools/probe_upstream_quic.py`使用官方aioquic状态机，无自制QUIC加密或握手。
单个同步探测，同时监听UDP数据和control TCP EOF，保持关联存活，处理
QUIC计时器和SOCKS IPv4封装，丢弃不匹配目标、FRAG/RSV/ATYP的报文。
握手成功后发送QUIC CONNECTION_CLOSE并关闭socket。只验证握手，不宣称
HTTP/3下载性能或ESP32路径通过。

aioquicAPI：https://aioquic.readthedocs.io/en/latest/quic.html

开发主机临时环境在`/tmp/esp32-quic-probe`，没有改变系统Python或固件：

```bash
python3 -m venv /tmp/esp32-quic-probe
/tmp/esp32-quic-probe/bin/pip install aioquic
/tmp/esp32-quic-probe/bin/python tools/probe_upstream_quic.py
```

后续决定：用户提出主动探测/暴露风险后，保留上游普通Vision配置，
不要求启用vision-udp443，也停止新增QUIC探测。以上工具和建议仅作为
历史诊断记录，未修改Windows/VPS配置。21:44卡顿时UDP443计数为0，
故该次卡顿不能归因于UDP443被上游阻断。

12f诊断age修正随后已包含在烧录的capacity14中。
不要把放行UDP443预先称为TCP零窗口、Wi-Fi兼容性或全部APP卡顿的修复。
