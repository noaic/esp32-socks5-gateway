# Phase 3 补充验证

本轮固件未修改、未重新烧录。仅增加主机测试和临时测试端点。

## HTTPS

Windows 直接 `curl.exe https://1.1.1.1/` 首次失败：证书吊销服务器离线。
串口确认 CONNECT443 成功，客户端 272 字节 / 上游 2632 字节后关闭，不能把它
记为完整 HTTPS 成功。

使用以下 Schannel 参数后，用户通过 `-v` 观察到 HTTP 301：

```cmd
curl.exe --ssl-revoke-best-effort -v https://1.1.1.1/
```

[官方 curl 文档](https://curl.se/docs/manpage.html#--ssl-revoke-best-effort)
说明此参数忽略 CRL 分发点缺失/离线导致的检查错误。该测试仍保留证书链和目标
身份验证，但不代表在线吊销检查已经成功；当前 UDP/DNS 尚未实现。

串口多次记录 `1.1.1.1:443`、CONNECT ready、正常双向 FIN，最后一次客户端
468 字节 / 上游 3335 字节。无本轮观察到的 crash/watchdog。
开发主机经同一上游预检 TLSv1.3、证书验证成功和 HTTP 301；此预检独立于板上测试。

## 主机事件循环

`python3 tests/test_socks_relay.py`：8 项通过；加上既有 packet rewrite 4 项，
主机共 12 项。大流量用例提升至 8 MiB，逐字节检查；新增 relay 空闲超时，以及
同一个生产事件循环在上游 RST 和握手超时后继续完成八个正常请求。

大流量最初触发了测试的缩短空闲超时：测试服务器用不可变 bytes 拼接产生平方级
复制开销。改为 bytearray.extend 后测试通过；生产事件循环没有因此修改。
主机测试不代替板上 RAM/任务栈/无线吞吐和真实 STA 断线恢复验收。

## 临时可控测试端点

`tools/test_stream_server.py` 是 WSL 主机工具，不加入 ESP32 firmware：

```bash
python3 tools/test_stream_server.py
```

监听 18080；不改变系统路由/防火墙。当前 WSL IPv4 `192.168.1.10`，地址可能
随 WSL 重启变化，需要重新预检。上游 SOCKS5 已实际 CONNECT 此地址成功。
主机经 SOCKS5 下载 `/8m`：HTTP 200、8388608 字节、SHA-256 正确。

Windows 下载和核对：

```cmd
curl.exe -o test.bin http://192.168.1.10:18080/8m
certutil -hashfile test.bin SHA256
```

期望 SHA-256：

```text
7d212b9c884f5c77896de960ae17cc341cda43b14d6a971f34ca29ebd4badf7f
```

上传同一文件并让服务器核对：

```cmd
curl.exe -T test.bin http://192.168.1.10:18080/upload
```

期望 JSON `bytes=8388608`、同一 sha256、`matches_8m=true`。
`/reset` 会故意中断一个声明长度为 8 MiB 的响应；`/` 返回 `proxy-test-ok`。
这些测试端点仅用于验证，本阶段不加入 Web UI 或 HTTP proxy。

板上下载/上传、真实网络断连及恢复结果待追加。

## 公网下载替代端点

用户指出 WSL 使用 NAT，本轮改用公网端点，避免测试机的私网路由影响测试。
[Cloudflare 官方测速组件](https://github.com/cloudflare/speedtest) 使用
`speed.cloudflare.com/__down` 下载测试数据。
本机解析并经指定 SOCKS5 预检 IPv4 `172.66.0.218` 的 HTTP 服务：HTTP 200、
Content-Length 8388608、完整读取 8388608 字节，没有重定向。该地址可能变化，
再次使用时应重新验证。此路径不访问 WSL、无需客户端 DNS。

```cmd
curl.exe -H "Host: speed.cloudflare.com" -o test.bin "http://172.66.0.218/__down?bytes=8388608"
```

这是 HTTP 应用请求中的 Host 头，固件仍仅按原始 IPv4:port 建立 SOCKS5 CONNECT。
公网下载检查完成字节数和传输速度；不套用 WSL 测试文件的 SHA-256。
受控文件哈希只适用于 `/8m`，不能拿来核对 Cloudflare 的响应数据。
板上公网下载完成：用户报告约 32 秒，curl 显示约 249k。
串口对应 `192.168.4.2:56162 -> 172.66.0.218:80`，CONNECT ready，
客户端请求 103 字节，上游响应 8389944 字节（包含 HTTP 头），双向 FIN，正常关闭。
约 33.8 秒为 SYN 到整个关闭完成的日志跨度，不能直接代替 curl 的下载用时。
此为功能/吞吐基线；未进行参数优化，不宣称已定位性能瓶颈。

临时 WSL 测试服务器已停止；本轮没有修改全局 ESP-IDF、固件或 Wi-Fi 参数。
本轮修改文件为 `tests/test_socks_relay.py`、`tools/test_stream_server.py`、
`docs/phase03-validation.md` 和 `docs/phase03.md`。
另一个故障候选端点 `1.1.1.1:1` 的主机预检显示：上游先返回 REP=0，发送
HTTP 请求后 5 秒内无响应，不能把它标记成明确的 SOCKS5 CONNECT 拒绝测试。
真实 STA 断线恢复、板上大流量和故障后恢复仍未通过验收。
