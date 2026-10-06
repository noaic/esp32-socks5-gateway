# ESP32-S3 公网 VLESS + REALITY 上游可行性

调研日期：2026-10-06。仅源码与资料调研，没有修改、构建或烧录运行固件，没有对用户 VPS 发起测试。

## 判断

有限并发客户端 PoC 在工程上值得尝试；直接运行原版 Xray 不适合当前 Xtensa/FreeRTOS 设备。要实现的是小型 C 客户端，复用 TLS/密码库与当前透明代理入口，不重新实现 TCP/IP。完整 REALITY + Vision + UDP 的成本明显超过替换 SOCKS5 请求头；现在没有实测依据保证原有 64 TCP/32 UDP容量或 24 Mbps 总吞吐。

收益主要是独立于局域网 Windows Xray，能够直接使用公网节点。它不是已证实的提速方案，也不能把握手成功等同于官方客户端的指纹与流量行为。

## 调研版本与证据

GitHub releases API 本次查询最近发布条目为 v26.9.30（2026-09-30，prerelease）；不能用 releases/latest 跳转得到的较老非预发布标签替代最近发布列表。v26.9.30 go.mod 使用 Go 1.27，REALITY依赖为 8cdf7bf9c7f0。当前 main 查询得到 Xray 7da5dae6502b787fc6d903863e9a6c5043d107a2（2026-10-05）、REALITY 3c98159dee388c2914a2b9e73ea57bc10023f9d3（2026-09-21）。这些描述的是调研版本，不是用户 VPS 的实装版本。[发布条目](https://github.com/XTLS/Xray-core/releases/tag/v26.9.30)、[依赖](https://github.com/XTLS/Xray-core/blob/v26.9.30/go.mod)。

用户原 Windows 26.1.18 依赖 REALITY e4eec4520535。核对两个 REALITY tls.go 后，旧实现接受纯 X25519；新版要求带 X25519MLKEM768 key share，并且在可选 X25519 之前。服务端的实际混合密钥交换取决于所选 group；不能凭此断言每次都协商混合算法，也不能发送伪造占位密钥并声称兼容。面向升级的完整客户端应实现真实的混合密钥生成/解封装。[官方变更](https://github.com/XTLS/REALITY/commit/8cdf7bf9c7f0)。

## 本项目资源事实

本机 sdkconfig：双核240MHz、16MB Flash、8MB Octal PSRAM；Wi-Fi Core0，TCPIP与TCP/UDP代理 Core1。serial50 bin939056字节；factory分区4MiB，并非16MiB全部是应用分区。

最新同一路径测速每种三轮：单路16.43/16.25Mbps，四路总吞吐24.49/21.25Mbps，均下载/上传顺序。测后内部heap free85807、历史minimum34332、largest36864；PSRAMfree6969452；维护任务栈历史剩余1032/6144。来自2026-10-05验收，是测后采样，不是TLS握手预算或今日实时值。

现有64TCP session，每session两份8192B固定PSRAM队列（合计1MiB，不含协议小缓冲与其它对象）；TCP元数据/flow table内部RAM。32UDPflow现用SOCKS独立association状态。以前诊断版本四路负载的Core1空闲中位约1%–7%，背景会话和观测开销不同；足以提示CPU余量有限，不能当作serial50当前CPU实测。

本机IDF的Mbed TLS为4.1.0。当前TLS1.3关闭，内部内存分配开启，输入16384/输出4096，动态buffer关闭，Curve25519配置开启。单看两份TLS buffer，64条连接即约1.25MiB，不含TLS状态、证书、密钥交换和握手临时内存；默认全用内部RAM显然无法维持此规模。需要显式PSRAM/自定义分配、限制握手数量，保留Wi-Fi/lwIP所需内部空间。8MB PSRAM不能代替内部heap和CPU。

S3有AES/SHA/MPI硬件支持，但不是把所有密码操作都硬件化：本机soc_caps没有ECC外设能力；X25519/Ed25519/ML-KEM需要评估实际软件实现，AES硬件支持也不代表整条GCM/TLS路径零开销。

## 协议工作量

| 层 | 可复用部分 | 新工作 |
|---|---|---|
| Transparent IPv4 TCP | rewrite、flow metadata、local listener、client lifecycle | 上游adapter与诊断目标适配 |
| VLESS | 有界队列、非阻塞socket | UUID/request/response framing；IPv4目的地址；明确encryption=none子集 |
| REALITY | 成熟TLS1.3与密码库 | ClientHello构造、key share关联、SessionID认证、特殊证书认证、版本兼容 |
| Vision | socket与端到端透明TCP | padding/command状态、内层TLS识别、外层TLS至raw切换、残余缓冲处理 |
| UDP | flow table、客户端归属、DNS预留、idle timeout | VLESS UDP framing或XUDP/Mux替代SOCKS UDP ASSOCIATE |
| 管理与健康检查 | UI、NVS试用回滚、事件/LED | upstream类型配置、UUID/SNI/public key/shortId、探测adapter与恢复默认值兼容 |

官方 REALITY UClient使用uTLS整理ClientHello，复用其key-share私钥计算X25519认证密钥，经HKDF和AES-GCM形成SessionID；服务器认证有HMAC与证书处理，TLS握手还必须正确验证CertificateVerify。不能仅设SNI、公钥或关闭证书验证。[客户端源码](https://github.com/XTLS/Xray-core/blob/main/transport/internet/reality/reality.go)。

本机ssl_client.c中ClientHello组装后进入transcript checksum更新。改写必须在正确的transcript位置生效，并同步session ID echo检查；只在socket send后改字节会导致TLS双方transcript不同。TLS输入/输出分片、WANT_READ/WANT_WRITE、失败清理、握手超时仍要正确处理。

官方REALITY使用Ed25519服务器公钥和CertificateVerify。本机库中可见Ed25519编号，但未找到可直接复用的完整Ed25519 PK/X509/TLS验证实现；编号不是算法支持。还没有适用的ML-KEM实现/配置。添加TLS1.3开关不足以完成互通。[服务端握手](https://github.com/XTLS/REALITY/blob/main/handshake_server_tls13.go)。

Vision会在满足条件时从外层TLS转入raw TCP，并处理已经被TLS预读的残余数据。HTTPS客户端仍自行完成端到端TLS，ESP32不解密用户HTTPS；raw模式不是明文暴露HTTPS，也不是去掉TCP/IP成本。非适用流量仍走外层加密，UDP亦不能套用此捷径。需要验证TLS/raw边界与close_notify；不要求复制Linux splice syscall。[Vision实现](https://github.com/XTLS/Xray-core/blob/main/proxy/proxy.go)。

服务器用户若配置xtls-rprx-vision，普通无flow TCP请求会被拒绝；无Vision的REALITY PoC需要单独允许无flow的测试账户，不能认为复用现有Vision账号即可。[入站检查](https://github.com/XTLS/Xray-core/blob/main/proxy/vless/inbound/inbound.go)。

VLESS普通UDP是长度封装的数据报流；官方Vision出站UDP转成Mux/XUDP路径，不能沿用SOCKS5的control TCP + 原生UDP relay。其UDP经TCP隧道时存在丢包导致的排队/队头阻塞。[UDP与flow选择](https://github.com/XTLS/Xray-core/blob/v26.9.30/proxy/vless/outbound/outbound.go)、[XUDP](https://github.com/XTLS/Xray-core/blob/main/common/xudp/xudp.go)。

## 公网性能约束

现有23,040B TCP接收窗口、17,280B发送额度、未启用窗口缩放。当前ESP32到LAN SOCKS的TCP ACK很近；Windows Xray负责独立公网TCP。直连后ESP32的TCP ACK来自VPS，接收窗口、发送额度、cwnd和RTT将直接约束性能。

以持续可用在途额度/RTT估算，不含丢包、慢启动、TLS、空口竞争，且不是实测预测：

| VPS RTT示例 | 23040B下载窗口上限 | 17280B上传额度上限 |
|---|---|---|
| 50ms | 3.69Mbps | 2.76Mbps |
| 100ms | 1.84Mbps | 1.38Mbps |
| 150ms | 1.23Mbps | 0.92Mbps |

100ms下单流20Mbps对应约250,000B有效在途数据；超过16位窗口，需要窗口缩放并规划实际接收与发送内存，不能只开一个选项。四流可合计更多，但每流加大buffer再乘并发数会占据大量PSRAM、增加lwIP对象和驱动压力。之前LAN扩大snd_buf出现TX内存压力，说明不能全局无差别扩容；WAN场景的收益需新测。[RFC7323](https://www.rfc-editor.org/rfc/rfc7323.html)。

直接转公网也会改变国内访问路径：现Windows Xray可在LAN直连国内IP；所有连接送海外VPS后，VPS再direct并不能恢复本地出口。B站延迟可能变差。这与是否支持REALITY是两个问题。不要借此自动加入本地GeoIP/GeoSite。

## TLS方案比较

| 方案 | 评价 |
|---|---|
| 原版Xray/sing-box | Go/Linux等运行时方案，不是可直接链接的Xtensa ESP-IDF组件；不做整核移植 |
| 修改本机Mbed TLS4.1 | 可复用IDF硬件/内存集成；缺口包含Ed25519、混合KEX、Hello与验证路径，工作量高 |
| wolfSSL官方ESP-IDF port | C库、TLS1.3/X25519/Ed25519及混合group支持，是优先做接口和构建验证的候选；没有现成REALITY适配，仍可能需要项目内patch |
| 自写TLS1.3/crypto | 不建议；扩大协议与验证责任，难以替代成熟库 |

wolfSSL主仓本次查询约2958 stars且活跃，提供官方ESP-IDF客户端、crypto test/benchmark；port README列明测试IDF5.2/5.3/master，不能据此保证6.1兼容。ssl.h定义X25519MLKEM768=4588，但必须检查实际build和双key-share构造顺序，不能把库支持混合group等同于REALITY兼容。密码算法库许可证及patch维护需随采用版本保留。[ESP-IDF port](https://github.com/wolfSSL/wolfssl/tree/master/IDE/Espressif/ESP-IDF)、[API](https://github.com/wolfSSL/wolfssl/blob/master/wolfssl/ssl.h)。

找到同类ESP32 VLESS项目yanxke/ESP32-VPN-Transparent-Proxy-Router：本次约4 stars，最近push2026-08-24，README明确TLS/REALITY unsupported。可参考VLESS framing，不是REALITY上板可用的证据，不作为替代当前项目的依据。[项目说明](https://github.com/yanxke/ESP32-VPN-Transparent-Proxy-Router)。

## 建议实验顺序与退出条件

1. 单独host PoC：固定服务端版本，核对用户flow/transport/maxTimeDiff，不触碰共享Xray。先确定TLS库能生成/更新所需ClientHello、关联key share私钥、认证REALITY证书、完成Ed25519 CertificateVerify；假证书与认证失败不得发送VLESS/应用数据。
2. 独立S3测试固件：先普通TLS1.3测试与真实crypto benchmark，再单条REALITY + VLESS TCP。记录握手CPU时间/墙钟、内部free/min/largest、PSRAM、峰值栈与文件大小；禁止直接以64TLScontext启动。
3. 接入现有透明入口：SOCKS模式保留；REALITY模式起始4–8总session、同时1个握手（实验值，不是最终容量），有界等待。固定少量worker。nonblocking handshake_step中的计算也可能长时间占CPU，必要时独立固定握手worker，不能一流一任务或让整个relay等待密码计算。
4. Vision单独验证：短记录、长记录、TCP分片、预读缓冲、半关闭、RST、非TLS TCP。对比plain outerTLS与Vision raw，测真实WAN RTT下吞吐与两客户端延迟。最终16/32/64路承诺由内存和并发结果决定。
5. UDP最后接：先DNS，再普通UDP/XUDP，最后QUIC；测TCP隧道丢包、关联重建和DNS隔离，不能因现SOCKS UDP成功就认为新路径也成功。

进入稳定开发前应能在目标IDF6.1中构建且官方Xray互通；握手失败无内存/状态泄漏；网页突发时管理页/DNS仍可响应；公网窗口调参不导致持续Wi-Fi TX内存失败。若TLS库缺口演变成自研TLS、资源余量不足或WAN吞吐不满足实际需求，应停留在实验分支，日常仍使用现有SOCKS方案。

绝对时间：REALITY hello包含Unix秒；服务端maxTimeDiff=0时源码不检查差值，因此NTP不是所有配置的硬性前提。非0时需要可信的当前时间来源和冷启动策略，可能要重新评估先前暂缓的时间同步；不应为了省实现默认关闭服务器检查。[REALITY配置](https://xtls.github.io/en/config/transports/reality.html)。

最终客户端若采用简化指纹、缺少官方fallback行为或不跟随新版握手要求，不应声称抗识别能力等同于Xray。接通测试、性能验收和指纹行为验证是不同证据。当前仅可行性分析，不包含公网抗探测验证。
