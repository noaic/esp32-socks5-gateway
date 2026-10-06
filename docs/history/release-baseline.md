# 可用基线与维护（2026-10-07）

当前固定基线是web51-operation-feedback；用户已确认重启验收可接受。AP最多两台，本次reset-network验收后当前管理入口为http://192.168.4.1/；之前自定义配置为192.168.10.1，已恢复默认。源码快照不包含板上NVS，不能据其恢复曾经的用户配置。硬件为ESP32-S3、16MB Flash、8MB Octal PSRAM、双核240MHz、GPIO48 WS2812。

## 本地完整快照

`build-snapshots/web51-operation-feedback/`保存源码、项目内lwIP覆盖、补丁、配置、依赖锁、bin/ELF、构建/测试/UART记录与SHA256清单；不会读取或备份板上NVS。sdkconfig含本机Wi-Fi凭据，快照仅用于本机恢复，不宜直接公开上传。

```bash
python3 tools/snapshot_manifest.py check build-snapshots/web51-operation-feedback
python3 tools/prepare_lwip.py check --idf-path /home/user/.espressif/v6.1/esp-idf
```

清单核对遗漏、额外文件和内容变化，不提供签名或来源认证。后续测试记录存项目根目录，固定快照保持不变。若需更新基线，应创建新快照，不覆盖清单。

ESP-IDF固定revision为fff9895c82d744c7237be8847347bdd1b07c6643，esp-lwIP为c6f2f878e7b0f86033214b85547d579be43351e3；cJSON1.7.19、led_strip3.0.3。prepare_lwip会拒绝未复审的IDF升级或非预期源码，不自动修补新版本。

## 重新构建

在快照目录或其副本执行。不要复制现有build目录，避免旧绝对路径缓存；依赖由锁文件及组件声明获取，首次构建仍可能需要网络。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
cd /home/user/esp/esp32-transparent-proxy/build-snapshots/web51-operation-feedback
python3 tools/prepare_lwip.py check
idf.py build
```

构建会新增build/managed_components等文件，因而该工作目录将不再满足原清单；建议复制到新目录再构建，保留原快照。重新编译带时间戳，bin哈希可能不同，不能据此直接认定功能不同。

## 烧录与恢复边界

普通idf.py flash使用当前分区表、应用和bootloader，现有分区不变时保留NVS。不要执行erase-flash；不能仅凭重新烧录恢复已改的口令/网络。RST/EN仅重启。当前应用bin945344字节，SHA256见快照summary.json，APP分区4MiB，剩余约77%。

串口使用115200 baud；烧录前结束采集，之后恢复同一连接交互采集。命令用法见serial50.md：reset-admin保留当前网络并清管理口令；reset-network保留当前管理口令并恢复编译默认网络。两者均有30秒一次确认、执行后重启、取消试运行；不能从RST触发。两条有效命令已在用户授权下板上实测，通过口令重设、口令保留及联网验收，见reset51-validation.md；不自动重复执行。

## 下一项实测：混合负载

保持固件与无线参数不变，仍使用http://192.168.1.10:18080/bench。先B单路、A空闲作为参照，再A播放一个固定视频、B单路。页面前台、屏幕点亮；分别记录视频清晰度、是否缓冲、两次吞吐与持续时间。结果自动回传，无需抄数。

记录两台MAC/IP/token、活动流与准入拒绝、内部RAM空闲/历史最低/最大块、PSRAM及维护栈；日常详细CPU与重传采样目前关闭，不能假称有此指标。仅在需要定位异常时临时开启诊断并单独标注其开销。历史最低值不会随测试清零，不能直接等同于本轮负载最低值；管理状态轮询也有开销，两组保持一致。

这项评估关注另一台业务是否被测速挤压、会话隔离和资源余量；准入配额不保证带宽公平。没有可复现问题时保留当前调度，不盲目增加连接容量。
