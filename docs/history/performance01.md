# 第一轮卡顿诊断与优化

## 判断

卡顿不能只归因于并发上限或无线硬件。此前 32 路版本的日志仍有
`SYN mapping unavailable`，且后段保护期映射表确实占满 128 条。
按已采集 map/expire 日志推算，部分失败出现时记录数约 93/114；
UART 数据可能不完整，不能把每次失败都确定为碰撞。
但源码明确只按 client IP/port 找映射：不同远端复用同源端口时拒绝，
这是必须修正的设计缺陷，而非 Wi-Fi 信道限速。

此前 8 MiB 客户端结果为用户报告 32 秒、249k。此次 WSL 直接经过同一个
192.168.1.2:7897 SOCKS5，下载同一 Cloudflare 大小数据得到：
8388608 bytes / 1.986059 seconds / 4223745 bytes_per_second（约 4.03 MiB/s）。
这是绕过 ESP32 的上游对照；两次不同时刻、客户端路径不同，
不能据此声称已准确量化无线/CPU占比或证明出口长期稳定。

ESP32-S3 AP+STA 使用同一无线信道，转发占用两侧空口时间，有实际吞吐上限。
本次启动 STA 信号约 -37 dBm，带宽 BW20、channel=13；信号强不能排除干扰和重传。
没有 LAN iperf baseline，尚不能宣称 249k 就是硬件上限。

## 修改文件与架构

| 文件 | 变化 |
| --- | --- |
| main/flow_table.c/.h（新增） | 独立 full 4-tuple 哈希表；每条映射固定内部源端口 20000+index；输出/accept 按该端口直接定位 |
| main/tcp_poc.c | 在输入重写 client source port，输出恢复真实 client port；512 条映射一次分配于 PSRAM；清理每秒一次，保留原有 2*MSL guard；packet 细节改 DEBUG |
| main/proxy_config.h | MAX_TCP_MAPPINGS=512；活跃 TCP=32、UDP=32 保持 |
| main/packet_rewrite.c/.h | IPv4/TCP checksum 按 RFC1624 字段增量更新；输入已验证后不重复扫描 payload；支持双向 peer-port rewrite |
| main/socks_relay.c | 双端 TCP_NODELAY；固定方向队列 2048→8192 字节；CONNECT 握手耗时、每 10 秒连接/字节/无线信号/栈统计 |
| main/udp_gateway.c | 固定 UDP worker 固定在 CPU1 |
| main/tcp_poc.c | 固定 TCP worker 固定在 CPU1，Wi-Fi/lwIP 使用 CPU0 |
| sdkconfig.defaults、忽略的 sdkconfig | CPU 240 MHz、PERF 编译、PSRAM优先用于Wi-Fi/lwIP、适度扩大窗口和 Wi-Fi 缓冲 |
| tests/test_packet_rewrite.py | 内部端口双向恢复，SYN/FIN/RST及payload不变，增量 checksum=0 边界 |
| tests/flow_table_harness.c（新增） | 512 个同 client IP/port、不同 remote 的隔离与容量、过期保护和哈希 unlink/reuse |
| README.md、本文件 | 更新当前实现与复测状态 |

输入举例：

```
192.168.4.2:50000 -> 1.2.3.4:443
192.168.4.2:50000 -> 8.8.8.8:443
```

分别交给 lwIP：

```
192.168.4.2:20000 -> 192.168.4.1:15000
192.168.4.2:20001 -> 192.168.4.1:15000
```

回包恢复两边真实端口。客户端看到的端点仍分别为 1.2.3.4:443、8.8.8.8:443。
没有改动 TCP sequence/ACK/window/flags；lwIP 继续负责所有 TCP 状态。
没有 PCB 元数据、逐流 task、NAT、分流或额外管理功能；原项目 lwIP patch 不变。

## 配置选择

参考本机官方 `examples/wifi/iperf/sdkconfig.defaults.esp32s3` 与实际 v6.1 Kconfig，
使用比单连接 iperf 峰值配置更温和的缓冲预算。没有复制禁用 watchdog、PHY调试配置，
没有改变 Flash DIO 或 Wi-Fi 国家/信道/带宽。

- CPU 160→240 MHz；CONFIG_COMPILER_OPTIMIZATION_PERF=y。
- SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y；本机 lwipopts.h 的 mem_clib_malloc/calloc 明确优先 PSRAM。
- Wi-Fi static RX=16、dynamic RX=48、static TX=16、cache TX=32、TX/RX BA window=16。
  PSRAM 优先模式下 Dynamic TX 不适用，按实际 Kconfig 选 Static TX。
- TCP send buffer/window 5760→11520；TCP receive mailbox=10，符合 window/MSS+2。
- TCPIP mailbox=48；TCPIP affinity CPU0；proxy TCP/UDP workers CPU1。
- 保留 watchdog、AMPDU、SO_LINGER、TCP/IP 完整校验；PSRAM 8 MiB 仍作启动 memory test。

增量校验参考 [RFC1624](https://www.rfc-editor.org/rfc/rfc1624)。
full payload 原始 checksum 仍校验；只减少改写后的重复计算。

## 验证

6 项 packet rewrite 测试通过；9 项生产 TCP event loop 集成回归通过；
新增原生产 flow table C harness（严格 -Wall/-Wextra/-Werror）通过。
其中包括 32 个并发会话、第 33 个等待/恢复、8 MiB内容正确性、EOF/半关闭、错误握手及重连。
本机缺少 ASan/UBSan 链接库，未完成 sanitizer 运行；普通严格编译及断言 harness 已完成。

v6.1 build 成功，无 warning/error；应用大小 0xbdfa0，4 MiB app partition余81%。
Flash 写入 hash 校验通过。monitor-perf01.log 确认 CPU=240 MHz、Flash16MiB、
PSRAM8MiB memory test、active32/metadata512、STA DHCP、UDP relay ready。
联网启动快照 internal_free=237555 / PSRAM_free=7719720 bytes；
随后 largest internal block=159744 bytes、TCP task stack free约6516 bytes。
以上是启动状态，不能证明满负载内存峰值或无泄漏。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check
idf.py build
python tests/test_packet_rewrite.py -v
python tests/test_socks_relay.py -v
gcc -std=c11 -Wall -Wextra -Werror -I main tests/flow_table_harness.c main/flow_table.c -o tests/.out/flow_table
tests/.out/flow_table
```

## Windows 最短对比

测试 Windows 连接 ESP32 AP，关闭浏览器后执行：

```cmd
curl.exe -4 -o NUL "http://speed.cloudflare.com/__down?bytes=8388608"
```

本次已用相同 SOCKS5 对该 URL 确认 HTTP200、Content-Length=8388608。
不用 HTTPS，避免将证书吊销延迟混入吞吐；随后再测正常浏览器网页多标签。
客户端结果尚待反馈，不承诺优化倍率。串口 stats 可区分：
active=32 长期饱和、handshake 堆积、metadata接近512、无线RSSI变化、内存/栈余量。
若持续达到容量或仍然较慢，应再做 AP+STA LAN baseline 与转发/上游对照，
而不是无界扩大并发和内部 RAM 窗口。

## 剩余限制

32 路活跃 TCP、512 保护期记录、32 UDP仍有边界；不支持 IPv4 fragmentation。
QUIC 未验证，大 UDP 限制不变。真实 AP/STA断线恢复与长时间稳定性仍需继续验收。
EOF/error-close语义沿用此前记录：lwIP SO_LINGER=0 不保证每次错误路径都生成 wire RST。

后续用户实测为361K/22秒，视频可用，多标签仍报ERR_CONNECTION_RESET；
已部署64路及错误操作/errno日志，最新状态见 [第二轮记录](performance02.md)。
