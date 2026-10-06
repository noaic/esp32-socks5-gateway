# Performance, resources and validation

## Short-term measurements

| Scenario | Download Mbps | Upload Mbps |
| --- | ---: | ---: |
| serial50, one client / one stream | 16.43 | 16.25 |
| serial50, one client / four-stream aggregate | 24.49 | 21.25 |
| web51, A idle / B one stream | 14.21 | 11.97 |
| web51, A playing 1080P / B one stream | 14.41 | 11.83 |

Only the last two rows form a same-session mixed-load comparison: approximately ±1% difference, with no reported stalls on A. Video can buffer and its actual bitrate was unknown; this does not prove fairness under sustained saturation. Different historical phones/background conditions also prevent attributing a throughput difference to management features.

ESP32-S3 has a single 2.4 GHz radio shared by AP and STA. Two clients do not double throughput. Wi-Fi/lwIP still need internal RAM despite ample PSRAM. Buffer, queue, core-affinity and IRAM experiments have produced both improvements and regressions; the baseline does not retain every attempted tuning change.

## Resource evidence

The tested local web51 application was approximately 945344 bytes; timestamps/configuration can change the image size. Its 4 MiB application partition had roughly 77% free. Linker data/bss were 22967/27928 bytes. At AP+STA startup, free internal heap/PSRAM were approximately 91231/6984320 bytes. A 16 MB Flash chip does not make all Flash available to the application, and total internal RAM is not the allocatable heap.

Mixed-load samples showed 76051 and 77751 bytes of free internal heap, with a largest block of 31744 bytes. The since-boot minimum of 25280 bytes had already occurred before that test. Maintenance-worker minimum free stack was 1032 out of 6144 bytes. These sampled/historical values do not capture every transient peak.

After network recovery, internal free/minimum/largest-block values were 79307/41620/34816 bytes, PSRAM free was 6962576 bytes, and maintenance-stack minimum free was 1084/6144 bytes. Minima across different boots are not direct evidence of a leak or performance change.

## Coverage and limitations

75 host regressions and frontend feedback/compatibility tests cover protocol/state/configuration behavior. Hardware validation includes TCP echo, SOCKS TCP/HTTPS, DNS/ordinary UDP, two-client admission and isolated reconnect, configuration rollback, Web/forwarding coexistence, LED control, Internet checks and valid serial recovery.

Complete QUIC compatibility, real upstream interruptions, mid-session UDP control failure, long-lived cached DHCP leases, long-term stability and Flash-write power loss still need dedicated coverage. A short successful benchmark is not evidence that all these cases pass.

Raw local logs and binaries are excluded from the public repository. Sanitized historical notes remain under `history/`. The publication copy uses example addresses, empty STA defaults and a portable UART test interpreter; its clean build/host regression are validated separately and are not flashed over the existing board's NVS configuration.
