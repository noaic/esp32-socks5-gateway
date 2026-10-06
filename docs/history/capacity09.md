# 第九轮：测速后 TCP 满员与缓存实验

## 已观测的问题

用户上一版 SpeedTest 下载4.19Mbps、上传2.31Mbps，随后报告Wi-Fi仍连接但网页打不开。
4.19Mbps约等于524KB/s，接近此前500K下载结果；没有证据表明达到ESP32-S3硬件上限，也不能单凭上游宽带套餐速度确定本项目这条代理路径的瓶颈。

打开串口后捕获到POWERON启动，断流前的状态未保存。因此后续正常启动不构成原故障已修复的证据。重新采集期间用户再跑测速，反馈暂时没有断流；`monitor-stall09.log`仍发现：

- 21:11:11、21:11:21两个10秒采样TCP活动64/64、handshake0；当时STA联网、AP客户端连接、DNS有回复、metadata约161～169/512、内部RAM约211KB，未见STA断线/panic/watchdog。
- 旧代码满员时不再accept，以backlog等待空位；旧连接正常空闲TTL120秒，无法区分HTTP/TLS keep-alive和其他无应用数据的会话。新网页可能排队到客户端连接超时。两个采样是明确的容量压力证据，不能证明用户所述每次断流都由这个原因引起。
- 同一窗口DNS往返有更长尾部：8个响应平均444ms、最大1835ms，association平均90825us；UDP/443发送157、回复0。外网路径/协议回退仍可能带来延迟。

## 修改

### 满员时按需求释放闲置连接

继续使用64个固定TCP session、单task/select、lwIP backlog和独立512 metadata。只有没有空位而且listener有新连接时，才优先回收最久未活动的合格session：

- 状态必须为RELAY，已经收到过上游应用数据（TLS初始响应也算，不能从这里识别完整HTTP事务）。
- 两个应用待发队列为空，至少30秒无读写活动。
- 先处理select返回的全部现有socket，再重新选候选；刚恢复读写的session不会按旧时间被回收。
- 使用写半部shutdown/正常close释放名额。shutdown错误仍走错误关闭；记录`capacity idle close`和累计`pressure_closed`。
- handshake、尚未收到首次上游数据、待发队列有内容或最近持续活动的session不作为候选。没有候选时仍使用有限backlog，不能保证超过容量的所有新请求立即完成。
- 满员且无候选时不把listener一直加入select，避免一个一直可读但不accept的listener造成忙循环。

普通空闲阈值仍为120秒；30秒压力回收是有新请求时的资源策略，不是新的一般TCP超时。它可能结束暂时静默的长连接/长轮询；无法凭字节计数精确知道应用是否期待未来响应，应用需要处理正常EOF/reconnect。

### 缓存配置实验

核对本机v6.1 `esp_system/port/soc/esp32s3/Kconfig.cache`后配置：

```
CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y
CONFIG_ESP32S3_DATA_CACHE_64KB=y
```

指令cache由16升32KiB，数据cache由32升64KiB，合计从可用内部heap转出48KiB；cache line仍32B。实际生成的sdkconfig.h确认0x8000/0x10000。
官方Octal PSRAM单流iperf性能档使用32KiB指令/64KiB数据cache：[v6.1性能说明](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/api-guides/wifi-driver/wifi-performance-and-power-save.html)。本项目不是单流iperf，缓存收益需要实测，不承诺倍率。
未一起增大每socket TCP窗口、连接数、Wi-Fi buffers、AMPDU、或启用额外IRAM。协议、PSRAM80MHz、CPU240MHz、AP客户端1和GPIO48保留。

## 文件与构建回归

- `main/socks_relay.c`：候选选择、按需压力回收、统计。
- `sdkconfig.defaults`：缓存选择；本机忽略的sdkconfig同步，不记录凭据。
- `tests/test_socks_relay.py`：64满员时新连接获得空位，最老idle连接干净EOF、活动连接继续echo；未等到首次响应的64连接仍受保护，新增测试以更长一般TTL避免两个测试计时阈值重叠。
- `README.md`、本文：验证、取舍与复现。

15项production TCP socket集成回归通过，含8MiB精确echo、双向背压、正常EOF、错误RST、原64并发/backlog、网络代际恢复及两个新容量测试。UDP核心未改，沿用上一轮13项通过结果，未声称本轮重跑。
本地lwIP patch完整检查通过，全局IDF未修改。
ESP-IDF v6.1 build成功，app0xc0990（788880bytes），4MiB分区余81%，未见编译warning/error。
ELF SHA256 `8dc901b48aa234484f4a849db930730dc56ca9354d1811edf66a617c4e583cc2`。
独立defaults reconfigure成功，`configure-capacity09-defaults.log`与/tmp/esp-proxy-capacity09-sdkconfig确认cache32/64、16MB Flash及Octal PSRAM。

```bash
source /home/user/.espressif/tools/activate_idf_v6.1.sh
python tests/test_socks_relay.py -v
python tools/prepare_lwip.py check
idf.py build
```

缓存回退：menuconfig -> ESP System Settings -> Cache，将Instruction cache设16KB、Data cache设32KB，同时将defaults改回对应选择后重建/烧录。已有sdkconfig不会被defaults自动覆盖。

客户端使用相同SpeedTest服务器复测，随后自然刷新首页/打开网页；测速与APP使用的混合负载无法当成单流带宽对照。持续观察`pressure_closed`、active/64、DNS延迟和internal RAM。UART工具`--no-reset`只表示不主动调用RTS reset，不能保证适配器打开串口时不改变控制线；本次已如实记录捕获到的启动，不把启动后的日志冒充原始断流现场。

## 新固件启动验证

`flash-capacity09.log`烧录成功、hash校验通过；`monitor-capacity09.log`ELF标识8dc901b48，确认16MiB Flash、8MiB PSRAM、AP+STA ready。初始空闲内部RAM约186KB，最大连续块139264bytes，符合缓存配置约48KiB的额外占用；PSRAM约6.63MiB。启动验证通过不代表已经提升带宽，需等待同条件客户端测速。

## 同轮客户端结果与压力回收

用户按相同测速方式反馈下载8.07Mbps、上传6.94Mbps，测后未发生断流。相对于上一版下载4.19/上传2.31Mbps，分别约1.93倍/3.00倍（增加92.6%/200.4%）。这是单次客户端报告；缓存与回收同轮改变、测速服务器和上游瞬时负载可能变化，不能把全部收益精确归因于某一个配置，也不能保证每次达到该值。

新板上日志再次达到64路，出现61次`capacity idle close`，统计`pressure_closed=61`，证明满员时的新请求确实触发了生产固件的回收分支；之后active降到18～23，继续有双向转发。关闭使用clean=1，未见STA断线/panic/watchdog。socket侧其他客户端/上游错误仍有，不把它们描述为已全部消失。
截至采集后段，最低内部空闲153451bytes（约150KiB），最大连续块降到114688bytes，最低PSRAM6882124bytes（约6.56MiB）。缓存转出内部RAM后当前负载仍有余量，极限并发和长时间内存趋势尚未验证。

UDP出现输入队列丢弃7、输出阶段丢弃11；flow/association/socket/delivery/callback等计数仍为0。计数没有逐包分类/输出err_t，不能确定全部都是DNS或都由Wi-Fi TX buffer造成；DNS输入与发送数的差值提示其中可能包含DNS突发排队。官方本机wlanif.c会把Wi-Fi transmit错误转换为ERR_MEM/ERR_BUF/ERR_IF，但当前未记录具体返回值，下一轮应先补充分类，再决定扩展DNS队列或增加有界的netif提交失败重试。
这些残留丢弃可能造成查询重试和首页延迟；本轮保存测速明显改善的配置，没有通过提高所有缓存/队列或屏蔽UDP443把剩余原因掩盖。

300秒采集完整结束：压力回收61次、最低内部空闲153451bytes、最小连续空闲块112640bytes（110KiB）、最低PSRAM6875632bytes（约6.56MiB）。本轮没有将短时间观察升级为长期无断流或无泄漏保证。
