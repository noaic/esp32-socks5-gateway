# perf22：普通 lwIP IRAM 对照

## 修改与架构

仅sdkconfig.defaults及本机sdkconfig开启CONFIG_LWIP_IRAM_OPTIMIZATION=y。保留CONFIG_LWIP_EXTRA_IRAM_OPTIMIZATION=y、L2_TO_L3_COPY=y和perf21所有无线、窗口、核心、队列以及CPU诊断设置，main数据路径不变。

先读本机v6.1项目本地components/lwip/linker.lf：普通选项覆盖socket/netconn、IPv4、pbuf、tcpip、ARP、UDP及同步调用等，与extra的TCP热路径分开。ELF确认lwip_send、lwip_select、ip4_input、pbuf_alloc由perf21 .flash.text迁移到候选.iram0.text，配置实际生效。

静态DIRAM从147198到158842bytes，新增11644bytes（11.37KiB）；.text从97871到109515bytes。链接可用182918bytes不等于联网后heap，PSRAM空闲也不能抵消内部RAM占用，需上板测量。

## 构建、部署和测试

build-perf22-lwip-iram.log编译成功，无编译器warning/error；size-perf22-lwip-iram.log保留占用；build-snapshots/perf22-lwip-iram/保存固件/config/main/manifest（含Wi-Fi配置，不分享）。单已有官方Kconfig无需镜像单测，沿用之前checksum/驱动包装回归，本轮用实际链接符号、编译、板上内存、吞吐、CPU与重传验证。

flash-perf22-lwip-iram.log记录烧录结果。自动测速服务标签perf22-lwip-iram，结果bench-perf22-results.jsonl，UART monitor-perf22-lwip-iram-live.log。Android刷新http://192.168.1.10:18080/bench，确认标签，点开始测试即可；结果自动保存。对照perf21两组中位数10.63/10.01和11.04/10.21 Mbps，优先比较同设置及两轮分布，不拿单轮最高值下结论。实测待采集。

## 风险和恢复

官方Kconfig说明普通选项在双核可能收益有限，这轮不预先保证提速；额外内部RAM占用必须与收益权衡。若没有重复收益或出现异常，关闭普通CONFIG_LWIP_IRAM_OPTIMIZATION，同步defaults和sdkconfig并重新构建烧录，保留TCP extra及RX copy；perf21快照可用作恢复点。不要为吞吐撤销已定位的RX池防积压修复。

app 807056bytes，ELF SHA256 34634e60680cbcde689d19845f0c063bd65222dfa18a7ee82f48818566982190。

## 第一组自动测试

三轮下载/上传Mbps为11.93/10.54、12.20/10.51、11.96/11.16，中位数11.96/10.54。相对perf21六轮合并中位数10.84/10.02约+10.34%/+5.23%；第一组看到了收益，待同固件第二组重复，不提前认定长期稳定。

23:31:22/32完整负载窗口，tcpip占用36.9%/38.3%，Core0空闲25.9%/23.1%；23:31:12是部分负载，不应拿30.2%作完整测速CPU值。RX held=0、peak1，TCP重传0，AP TX mem计数4在两个完整窗口不增长。最低内部heap采样135115bytes（131.95KiB）、最大块86016bytes，PSRAM约6.58MiB。Flash hash验证通过；结果及CPU摘要已保存候选results.json。

## 第二组复测及最终保留

第二组三轮下载/上传Mbps：11.89/10.74、11.62/10.89、11.53/10.40，中位数11.62/10.74；两组六轮合并中位数11.91/10.64，相对perf21六轮合并10.84/10.02约+9.94%/+6.21%，相对rx17最初10.02/9.10约+18.9%/+16.9%。保留普通和TCP extra两个IRAM选项及当前固件，不把这些短期LAN实验直接等同公网提速。

第二组完整负载tcpip36.1%—37.5%、Core0空闲24.1%—26.8%。两组RX held=0/peak1、TCP重传0，AP TX mem第二组没有增长（累计4）；内容校验及自动结果保存通过。第二组内部heap采样约142259—142843bytes，最大块86016bytes；第一组最低135115bytes。两个IRAM选项相比perf20总共增加25276bytes（24.68KiB）静态内部内存。仍需并发应用及长时间验证，不能把短测速当作泄漏/卡死的长期证明。

当前源码/config与板子均为perf22。修改文件为sdkconfig.defaults及本机sdkconfig，新增本阶段docs、构建/尺寸/烧录日志和快照；main源码未改。后续候选优先考虑代理任务热数据的RAM布局，必须先读结构及内存生命周期，固定PSRAM payload不能盲目全搬内部RAM。
