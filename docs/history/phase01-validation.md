# Phase 0/1 板上验收记录

2026-10-02，ESP-IDF v6.1，用户确认的 UART 串口 `/dev/ttyACM0`。

## 本次变更

- `sdkconfig.defaults`：console 改为 UART0/115200，无 secondary console。
- 本地忽略的 `sdkconfig`：上游 SSID UPSTREAM_WIFI 和用户提供的密码；凭据不进入源码默认配置。
- `tools/uart_capture.py`：有时限的 UART 日志采集，esptool 标准 RTS 复位，不改固件。
- `README.md`、本记录：串口配置、权限和板上结果。

APSTA 应用架构保持原实现：默认 netif/事件循环，定时重连，单 AP client，
不启用转发/NAPT。没有进入 Phase 2，也没有修改 esp-lwIP。

## 构建与烧录

`idf.py build` 成功，无编译 warning/error，最终固件 760,624 bytes。
`idf.py size` 成功；image-info 显示 ESP32-S3、16 MB Flash、DIO/80 MHz，
checksum/hash 有效。

首次端口权限失败未写入。用户调整设备节点 owner 后，
`idf.py -p /dev/ttyACM0 flash` 成功，hash 校验通过并执行 RTS reset。
esptool 识别 ESP32-S3 revision v0.2、双核、8 MB Embedded PSRAM。

USB 描述符为 VID 1a86 / PID 55d3 / USB Single Serial，符合 UART 适配器通路。
IDF monitor 此次没有采集到启动输出；直接 UART 脚本采集成功，确认实际应用
UART 引脚为 GPIO43/44。原始日志：`flash-phase01.log`、`monitor-phase01.log`。

## 已验证结果

| 检查 | 实际日志/结果 |
| --- | --- |
| Flash | Bootloader 16 MB；应用 `Flash=16777216 bytes` |
| PSRAM | Octal 8 MB，80 MHz；`SPI SRAM memory test OK` |
| 双核 | `cores=2`，Multicore bootloader/app |
| 硬件校验 | `PSRAM=8388608 bytes initialized=yes` |
| AP | `ESP32-Transparent-Proxy`，192.168.4.1，max_clients=1 |
| DHCP server | `DHCP server started on interface WIFI_AP_DEF` |
| STA | UPSTREAM_WIFI，WPA2-PSK，channel=13，RSSI -35 dBm |
| STA DHCP | 192.168.1.103/24，gateway 192.168.1.1 |
| 状态 | `AP=up STA_IP=yes ready=yes` |
| 子网 | AP 192.168.4.0/24、STA 192.168.1.0/24，不重叠 |
| ready 时 heap | internal_free=278895 bytes，PSRAM_free=8385644 bytes |
| 短时观察 | 90 秒 UART 采集完成，未见 panic/watchdog/STA 断线；未见 AP client 事件 |
| 客户端（用户反馈） | Windows 192.168.4.2，ping 192.168.4.1 正常，约 3 ms |

## 尚待验证与限制

用户后续确认 Windows 客户端为 192.168.4.2，ping AP 正常，约 3 ms。
设备侧 DHCP lease 日志、加入/离开、第二客户端限制和上游断线重连尚未完整验收。
短时无异常不能证明长期无内存泄漏/无 watchdog。当前没有外网转发，ready
只代表接口状态，客户端“无互联网”为预期。

上游实际信道为 13，AP 自动跟随；不支持信道 13 的客户端可能搜不到该 AP。
UART 已可读，不再以 `/dev/ttyACM0` 名称推断为 ESP32 原生 USB。
Wi-Fi power save 日志为 type=1，沿用 IDF 默认，Phase 5 再对比调优。

下一步先完成上述客户端/重连验收，再做单 flow transparent TCP echo PoC。
