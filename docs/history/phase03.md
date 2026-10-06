# Phase 3：透明 TCP 到 SOCKS5 CONNECT

## 结构

客户端原始 IPv4 TCP 四元组 -> 项目已有 IPv4 input rewrite -> lwIP 本地 listener
-> 一个固定 proxy_tcp task / 非阻塞 select -> 上游 SOCKS5 CONNECT。
回包继续通过已有 output rewrite 恢复原始目标 IPv4/port。

lwIP 负责两侧 TCP；映射仍独立保存，不修改 PCB，不实现 TCP 状态机。
本阶段没有新增 lwIP patch，全局 IDF 安装不变。

`main/tcp_poc.c` 保留 packet rewrite、32 条静态映射及 2*MSL 关闭保护。
`main/socks_relay.c` 是实际生产事件循环，同时可直接在 Linux 主机编译测试。
握手状态依次为 nonblocking connect / greeting TX/RX / CONNECT TX/RX / relay。
每个方向仅有 2048 字节固定队列，未发送完就暂停该方向读取，形成背压。
握手总超时 15 秒，转发无活动超时 120 秒。EOF 在排空队列后传播 SHUT_WR，
两侧 EOF 且队列为空才正常 close；失败和超时设置 SO_LINGER(1,0) 后关闭连接。
注意本机 lwIP 的零 linger 仅在存在 unsent/unacked 数据时强制 tcp_abort；
其余情况可能发 FIN。因此不保证所有失败都以 RST 通知客户端，Linux 主机测试的
RST 结果也不能证明板上有相同语义。串口 `reset=1` 表示选择错误关闭路径，
不是抓包确认已经发送 RST。
固定任务栈 8192 字节，队列在任务栈内，无逐连接任务或应用层动态队列分配。

协议依据 [RFC 1928](https://www.rfc-editor.org/rfc/rfc1928)：
`05 01 00` 请求 NO AUTH；只接受 `05 00`；CONNECT 使用 ATYP=1 原始 IPv4。
只接受 IPv4 BND.ADDR 回复，ATYP=3/4、非零 REP、异常 VER/RSV 均失败并关闭。
分段握手逐字节累计；CONNECT 回复只读取精确 10 字节，避免吞掉紧随其后的数据。
本机 IDF transport 的 SOCKS proxy 是 SOCKS4，不能直接满足本阶段要求。

## 范围和已知限制

- **一个活动 TCP 连接**，32 条映射包含关闭保护期中的记录，不代表 32 个并发连接。
  使用一个 local listener，源 IP/port 相同而原始目标不同的连接会被拒绝，避免四元组折叠。
  后续并发扩展需要不同本地 listener port 或另一种明确保留四元组的映射方式。
- SOCKS 模式捕获 AP 客户端发往外部单播 IPv4 的 TCP，包括 STA 所在 LAN；
  ESP32 自身 IP、AP 子网、组播、广播、回环与保留地址不捕获。
- IPv4 options、fragmentation 不支持；TCP options/payload 保留，校验和重新计算。
- 客户端 TCP 握手先于上游 CONNECT 完成；上游拒绝后关闭客户端连接，可能为 FIN 或 RST，不能承诺严格 RST 传播。
- 仍没有 UDP/DNS，普通网页浏览涉及 DNS、并发 TCP 或 QUIC，不能作为当前完整网关验收。
- STA 未 ready 或中途断线时，上游连接失败/超时；重连后可发起新的连接。
- 32 个 slot 全处于活动/保护期时，新流被丢弃；不是高频连接或完整软路由方案。

## 配置、构建与回退

上游为 `main/proxy_config.h` 的 IPv4/port。
`menuconfig -> Transparent TCP gateway -> Forward TCP through SOCKS5` 默认打开。
关闭该选项即可回退 Phase 2，仅 `198.51.100.7` TCP 被送到本地 echo。
IPv4 hooks 的旧 `PROXY_POC_ENABLE` 名称保留，避免无必要改变已验证 patch。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
cd /home/user/esp/esp32-transparent-proxy
python tools/prepare_lwip.py check
idf.py build
idf.py -p /dev/ttyACM0 flash
python tools/uart_capture.py --seconds 3600 --log monitor-phase03.log
```

原生 gcc 测试从未激活 IDF 的新 shell 运行。EIM 激活后 PATH 中的交叉工具 `as`
可能干扰原生 gcc，因此不要在该 shell 直接执行主机测试。

```bash
python3 tests/test_packet_rewrite.py
python3 tests/test_socks_relay.py
```

第二项使用本机 loopback sockets；受限制沙箱需要授权。6 个测试执行生产 C 循环：
拆分 greeting/CONNECT 回复、262144 字节完整传输及提前 FIN、回复+payload 合并、
认证拒绝、错误 REP/ATYP/VER、握手超时、上游 RST。packet rewrite 的 4 项测试通过。

## 板上测试

用另一台 Windows 连接 AP，保留开发电脑正常上游网络。
无需脚本，不要指定手动代理，不依赖当前客户端 DHCP 地址：

优先使用用户已有的 ncat：

```cmd
ncat -v 1.1.1.1 80
```

连接后输入 `GET / HTTP/1.0`，连续按两次 Enter，观察 HTTP 响应。
没有 Host 头时服务端可能返回 400；HTTP 响应仍能证明请求/回包链路，
但应同时核对串口 CONNECT 和原始目标日志。以下 curl 命令是可选替代。


```powershell
curl.exe -4 --noproxy "*" --connect-timeout 10 --max-time 25 -v http://1.1.1.1/
```

不加 `-L`，预期 HTTP 301 响应即可证明 TCP 请求和响应经过真实上游。
该请求只是 HTTP 应用流量测试，固件没有 HTTP proxy。
当前 SOCKS 模式下旧 `198.51.100.7` ncat echo 测试不再有效。

串口应显示原始目标 `1.1.1.1:80`、`SOCKS5 CONNECT ready`、双向字节数与关闭。
主机经指定上游实测：NO AUTH 回复 `0500`；IPv4 CONNECT 成功；HTTP/1.1 301。
主机结果仅确认上游/目标可用，不能替代板上透明转发验收。

## 当前构建结果

ESP-IDF v6.1 构建成功，无编译 warning/error；app size `0xbfb20`，4 MiB app
分区余量 81%。16 MB Flash / Octal PSRAM 80 MHz 配置不变，SO_LINGER 已开启。
烧录并校验成功；板上启动确认双核、16 MB Flash、8 MB PSRAM / memory test OK，
STA `192.168.1.103`，AP `192.168.4.1`，AP+STA ready，无启动 crash/watchdog。
板上基础链路验收通过：Windows 客户端使用 `curl.exe http://1.1.1.1/` 收到 301。
串口记录：

```text
192.168.4.2:56151 -> 1.1.1.1:80 <-> 192.168.4.1:15000
SOCKS5 CONNECT ready flow=0
input FIN flags=0x11
output FIN flags=0x11
closed flow=0 reset=0 client_bytes=70 upstream_bytes=386
```

客户端请求 70 字节，上游响应 386 字节，双向 FIN 与正常关闭已观察到。
本次没有 crash/watchdog；仅是一条短连接，不代表大流量、断网恢复、长期稳定性
或内存泄漏已经完成板上验收。主机测试与板上测试的覆盖范围应分别看待。

## 本阶段修改文件

- `main/socks_relay.c`、`main/socks_relay.h`：非阻塞 SOCKS5 与双向 relay。
- `main/tcp_poc.c`：复用透明 rewrite/mapping，加入 SOCKS 模式及周期映射回收。
- `main/proxy_config.h`：上游 IPv4/port。
- `main/Kconfig.projbuild`、`main/CMakeLists.txt`：模式选择与新源文件。
- `sdkconfig.defaults`：默认 SOCKS 模式与 SO_LINGER；本机忽略的 sdkconfig 同步配置。
- `tests/relay_harness.c`、`tests/test_socks_relay.py`：生产事件循环的主机集成测试。
- `README.md`、`docs/phase03.md`：配置、测试步骤、证据与限制。

建议先补板上 HTTPS、大流量和失败/超时测试，再进行 UDP ASSOCIATE / DNS，
最后扩展并发与性能测试。不要把当前的 HTTP 301 当作完整浏览器上网验收。

补充 HTTPS / 大流量 / 故障恢复记录见 [phase03-validation.md](phase03-validation.md)。
