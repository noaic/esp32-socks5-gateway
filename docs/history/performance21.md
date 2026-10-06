# perf21：lwIP TCP 热路径 IRAM 单参数对照

## 修改文件与架构

sdkconfig.defaults及本机sdkconfig仅启用CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION=y，普通CONFIG_LWIP_IRAM_OPTIMIZATION仍关闭。保留perf20的运行时间诊断及原窗口、Wi-Fi、任务核心、接收copy修复。main数据路径源码与perf20相同，lwIP继续处理TCP状态机，没有改PCB或引入新task。

依据本机ESP-IDF v6.1 components/lwip/Kconfig和linker.lf（项目本地副本同版本），extra选项将tcp_input、tcp_write、pbuf_clone、checksum、部分内存函数等搬到IRAM。Kconfig估计约17KB；本项目实际链接DIRAM .text从84239到97871bytes，增加13632bytes（13.31KiB），静态DIRAM总用量从133566到147198bytes。链接剩余194562bytes不是联网后的运行时heap，仍须UART确认。perf20近期内部heap空闲约169—170KB。

ELF实际符号验证：tcp_input/tcp_write/pbuf_clone/inet_chksum均由perf20的.flash.text (0x420...)变为候选.iram0.text (0x403...)，证明不是仅改配置却未生效。普通lwIP IRAM仍未开启，避免两项混测。依据CPU诊断tcpip约45%、Core0忙约84%选择此实验，不能提前保证提速。

测速主机工具另加--label；/bench将标识注入页面，报告含页面标识，后端还独立保存服务端firmware_label，避免跨固件混淆。此标识由操作员指定，不是设备自动读回固件，切换必须同步重启服务；本次已同步为perf21-tcp-iram。先前perf20服务端记录无标识，但有独立结果文件和UART窗口。

## 构建与测试

build-perf21-tcp-iram.log构建成功，无warning/error；size-perf20-profile.log与size-perf21-tcp-iram.log保留静态内存对照；flash-perf21-tcp-iram.log Flash hash校验通过。build-snapshots/perf21-tcp-iram/保存固件、配置、main源码和manifest，含本机Wi-Fi配置不可分享。

串口monitor-perf21-tcp-iram-live.log；主机服务/tmp/esp32-perf21-peer.log；自动结果bench-perf21-results.jsonl。经现有Windows SOCKS5 GET /bench验证页面标签正确、URL可达。只改变一个已有官方Kconfig选项，不增加镜像单测；原perf20 checksum/诊断回归已通过，此轮以实际符号、构建、上板内存/吞吐/CPU/RX held对照验证。

Android连接AP、关闭移动数据、暂停其他视频/下载、刷新http://192.168.1.10:18080/bench，确认perf21-tcp-iram，点一次开始测试。三轮各8MiB下载/上传并校验，结果自动记录。与perf20中位数9.65/8.90及rx17 10.02/9.10比较，保持诊断设置一致优先比perf20，不将单次波动当作提升。尚待本轮真实数据。

## 风险与回退

IRAM占用与可用内部RAM共享资源；PSRAM空闲不能抵消内部RAM不足。若无稳定、可重复收益，关闭extra选项同步defaults及sdkconfig，重新编译/烧录，或恢复perf20快照。原RX缓冲修复保持开启，不撤销。统计快照每10秒会暂停调度约1ms，任务运行时间为采样近似；继续监视内存、RX held、TCP重传和异常，不用增大窗口掩盖瓶颈。

app 806112bytes，ELF SHA256 06c0ceb3266e1fa3783a3f0c351fe9633c5d4137e643882b0cdaf28735eb7030。

## 第一组自动实测

23:23:41—23:24:20，三轮下载/上传Mbps：10.54/10.01、10.63/9.76、11.49/10.02；中位数10.63/10.01。相对同诊断设置perf20为+10.16%/+12.47%，相对rx17原基线为+6.09%/+10.0%。内容校验通过，自动start/round/complete均已保存。

完整负载窗口23:23:55、23:24:06、23:24:16：lwIP约41.0%—41.9%、wifi37.7%—39.0%，Core0空闲18.9%—21.0%、Core1空闲48.9%—50.5%；速度更高而lwIP占用降低，支持IRAM减少处理成本的解释，但单次不能排除无线波动。内部heap最小采样149531bytes（146.0KiB）、最大块106496bytes、PSRAM约6.57—6.60MiB；RX held=0、peak=1、TCP重传0。AP TX mem错误从6到8（增加2次），没有放大为持续停流，仍持续监视。

保留当前固件待第二组同页重复，不在两组之间改变配置。results.json保存首组结果和CPU样本；第二组未完成前不宣称长期稳定或重复收益。

## 第二组复测与最终决定

23:25:10—23:25:49同一固件再测，三轮下载/上传Mbps：11.59/9.98、9.16/10.33、11.04/10.21；中位数11.04/10.21，相对perf20为+14.40%/+14.72%。六轮合并中位数约10.84/10.02，但下载仍有9.16—11.59的波动，避免只取最好一轮。

第二组全负载窗口lwIP41.5%—41.8%、Core0空闲18.3%—19.1%，支持第一组CPU趋势。RX held=0/peak1，TCP重传仍0；AP TX内存错误第二组增加3次（累计8到11），没有观察到持续停流；内部heap采样约153719—155467bytes、最大块106496bytes。第二组同样自动保存、内容校验通过。

决定保留CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION=y及当前候选固件；保留CPU诊断和RX copy修复。本次仅两组短期受控LAN测试，不证明长期稳定或公网同幅提升。下一步可评估普通lwIP IRAM/代理热点内部RAM布局，但必须分别做对照，不能与此轮混测。README及旧历史记录中的固件状态以本段为当前状态。
