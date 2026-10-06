# 浏览器访问及断线恢复修复

## 现象和证据

旧固件只有一个活跃 SOCKS5 TCP relay。串口显示 DNS 多次返回后，
其他并发 TCP SYN 完成 SYN-ACK 随即被关闭；不能将网址访问失败全部归因于 DNS。
客户端已确认 DNS（UDP/53）和 NTP（UDP/123）成功，QUIC 未验证。
AP 客户端多次离开 reason=1，日志没有足够证据确定无线断开的根因。
用户后一次 DNS timeout 已确认为客户端 Wi-Fi 断开。

AP DHCP option 6 已下发 1.1.1.1，gateway 为 192.168.4.1。
本机 v6.1 esp_netif_set_dns_info_api 对 DHCP-server 接口单独调用
 dhcps_dns_setserver_by_type，不会被 STA DHCP 获取的上游 DNS 覆盖。
ESP32 不监听 DNS/53。访问公网 DNS 的 UDP 和 TCP 均经 SOCKS5；
因此公网 DNS 可用性取决于上游代理和出口。无需实现本地 DNS 服务。

## 修改

- main/socks_relay.c/.h：单 select loop 支持 32 个并发连接，各自维护
  handshake、两方向固定 2048 字节队列、EOF、超时及统计。保留固定 task。
  固定会话缓冲在 PSRAM 一次分配，原有握手/关闭语义不变。网络和客户端事件通过原子 epoch 通知 worker，
  worker 主动关闭旧 socket，避免等待 120 秒 idle timeout。
  容量满时暂停 accept，让 lwIP bounded backlog 等待空位，避免 accept/RST 重试风暴。
  其他拒绝路径明确打印原因，并 retire 已 claim 的映射。
- main/tcp_poc.c、main/proxy_config.h：listener backlog=32，
  静态映射表从 32 增至 128（包括关闭后的 2*MSL 保护记录），
  不缩短 lwIP TIME_WAIT，不把元数据写入 PCB。增加映射失败限频日志。
- main/udp_gateway.c：客户端离开只清理 UDP flow，保留上游 control/association；
  STA 断开才使 association 失效，重新获取 IP 后立即重建。
  client generation 和 network generation 分开，旧队列及回包不能投递新客户端。
  控制 TCP keepalive idle=15s、interval=5s、count=3，避免依赖默认数小时探测。
- main/app_main.c：传递 STA/IP/AP 事件给 TCP worker；输出 DHCP DNS 和 STA RSSI；
  esp_wifi_set_ps(WIFI_PS_NONE) 保持网关无线唤醒。这是稳定性调整，不能证明无线根因已消除。
- sdkconfig.defaults、忽略的 sdkconfig：MAX_SOCKETS=104、MAX_ACTIVE_TCP=80，
  UDP PCBs=40。socket 预算包含 32*2 TCP、32 UDP、control/listener及系统余量。
  本机 newlib sys/select.h 明确允许用户定义 FD_SETSIZE；v6.1 lwIP Kconfig 要求超过 61 个
  sockets 时扩大它。因此 CMakeLists.txt 对所有 IDF source components 统一定义
  FD_SETSIZE=128，lwIP offset=24，保持 app/lwIP/VFS 的 fd_set ABI 一致。
  检查 949 条组件源码编译命令全部包含此定义；唯一例外是生成的 ELF 元数据源码，
  不使用 select/socket API。不修改全局 toolchain/header 或 IDF。
- tests/relay_harness.c、tests/test_socks_relay.py：新增 32 个同时保持打开的
  echo 会话、满额第 33 条等待后获得空位、客户端 reset、网络 down/up 和同进程恢复测试；保留原有回归。
- README.md、本文件：同步已验证与待验证状态。

## 本机验证

生产 TCP event loop 回归 9 项通过；packet rewrite 校验 4 项通过。
ESP-IDF v6.1 构建成功，无编译 warning/error；镜像 0xc1d30，4 MiB app 分区余 81%。
烧录写入 hash 校验通过。monitor-recovery32.log 确认 Flash 16 MiB / PSRAM 8 MiB memory test、
DHCP DNS=1.1.1.1、TCP active=32 metadata=128、STA 192.168.1.103、
RSSI=-34 dBm、PS=NONE、UDP relay 192.168.1.2:7897 ready。
新版本联网启动快照 internal_free=252883 bytes / PSRAM_free=8218428 bytes，不是泄漏判定。
本次客户端网页和断开重连验证尚待反馈；不能宣称真实无线断线全部修复。
构建日志 build-recovery.log，烧录日志 flash-recovery32.log，UART capture 正在运行。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tools/prepare_lwip.py check
idf.py build
python tests/test_socks_relay.py -v
python tests/test_packet_rewrite.py -v
```

## 板上复测

客户端断开再连接 ESP32 AP，获取新租约，在 Windows 运行：

```cmd
nslookup example.com
curl.exe http://example.com/
```

第一条应显示 Server Address=1.1.1.1 并返回解析；第二条验证域名解析后 TCP。
浏览器同时打开多个网页，再断开/重连客户端，核对 TCP network/client reset、
UDP association=kept、重新建立 TCP 及 DNS 回包。
真实 STA 故障恢复及长时间 AP 稳定性仍需板上测试，主机测试不能替代无线测试。

## 板上中间反馈

8 路中间版本用户确认 DHCP DNS=1.1.1.1、单个 example.com 网页能打开，
多个标签明显卡顿。串口在约 45 秒内记录 128 个 SYN 映射和 98 次容量拒绝；
这是将实际活跃连接扩到 32 并停止满额 accept 的依据，不把中间版本当作最终修复。
保留 monitor-recovery.log 作为原始证据，新版本使用 monitor-recovery32.log。

## 剩余边界

32 个活跃 TCP、128 个保护期映射、32 个 UDP flow 仍有容量限制。
多网站后台连接超过 32 时先进入有界 listen backlog 等待，资源仍然有限。
关闭映射保护保留原始 endpoint 和 FIN/ACK 重传语义，不可无条件清空。
同一客户端源 IP/port 连接多个不同 remote 的共享 listener 冲突仍是已知限制。
只支持 IPv4；客户端 IPv6 及浏览器 DoH 行为不由此 DNS 下发决定。
大包/分片 UDP 限制、上游代理自己断流等未在此次修复中解除。

后续版本已修正共享源端口冲突，映射扩至 512、转发与缓冲调整见 [第一轮优化](performance01.md)。
本文件的 128 条表和 2048 队列等参数是恢复阶段历史配置。
