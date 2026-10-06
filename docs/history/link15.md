# link15：卡顿现场的两侧本地 ICMP 对照

capacity14在21:44复现卡顿：应用36路，PCB峰值148，分配失败0。
客户端TCP零窗口、两侧接收安静仍出现。容量未触顶，增加PCB上限
没有解决本次问题；尚未确认无线驱动、客户端或上游为根因。

## 修改

- main/link_probe.c、.h：使用本机ESP-IDF6.1官方esp_ping组件。
- main/app_main.c：初始化诊断任务，DHCP分配客户端IP时请求基线探测。
- main/tcp_poc.c：原有积压trace请求触发现场探测，TCPIP线程只投递
  非阻塞单槽队列；没有在TCPIP线程执行ping或等待。
- main/CMakeLists.txt：加入link_probe.c。
- docs/capacity14.md、upstream-quic13.md：补充实测与停止QUIC探测的决定。

两侧同时各发送3个16字节ICMP数据包，间隔/超时500ms；最多每30秒
一轮，重复请求合并/丢弃。AP目标取自已有flow或DHCP事件，STA目标
只取当前网关，校验同子网且绑定对应netif；不探测VPS或公共网站。
保留之前无线、转发队列与PCB256配置，不增加应用连接数。

一个固定3072字节栈协调任务，最多同时两个官方ping任务/两个raw
socket。官方默认ping栈为2048+TASK_EXTRA_STACK_SIZE，另有任务控制块、
socket、raw PCB和少量包/队列内存。结束后删除session，官方实现
异步退出并释放资源；30秒冷却大于其1秒退出检查，不累计每流任务。

## 观察方式与限制

先看正常接入时AP-client/STA-router的reply及finished，再与卡住时
结果比较。两侧都回应时仍需检查TCP窗口/应用与上游，单侧失联时
能缩小范围；手机不回应ICMP本身不能证明Wi-Fi故障。
ICMP会唤醒无线端点，可能改变省电时序，因此这是临时诊断，并非
修复或正式保活策略。每10秒采样一次积压和30秒冷却也可能漏掉
短暂卡顿；不能拿恢复后的成功探测证明故障时链路正常。

Android保持移动数据关闭，正常使用APP；卡住时保持Wi-Fi连接并
报告时间。无需脚本、测速或修改Xray/VPS。

## 构建与部署

- build-link15.log：构建成功，无编译warning/error；app800864bytes。
- test-link15-patch.log：项目本地lwIP patch校验通过。
- test-link15-observer.log：原wrapper/age回归通过；不是新增ping功能的
  主机仿真测试。首次主机回归受IDF PATH中交叉as影响，使用系统PATH重跑通过。
- flash-link15.log：烧录hash校验通过。
- 快照build-snapshots/link15-local-ping/，ELF SHA256
  35bf0044a2d54e8f57c126a29d8b922d694ca2f9458cb669e0666a70a785d151。
- monitor-link15-live.log：持续UART采集；现场卡顿结论需实际复现，
  尚未证明根因或解决问题。

恢复上一轮可使用build-snapshots/capacity14-pcb256/快照。快照含本机
Wi-Fi配置，不分享。移除诊断的源码方式：删除app_main初始化/DHCP
请求、tcp_poc的request、对应include以及CMake的link_probe.c条目后构建。

## 上板基线（21:58:43—45）

Android接入192.168.4.2，两侧session均start=ESP_OK。
AP-client发送3/收到3，RTT70/3/1ms；STA-router发送3/收到3，
RTT1/1/4ms。正常接入时手机和网关均响应ICMP，可与下一次卡顿
对比。首个手机70ms发生于接入阶段，不能仅凭这一包认定故障。
