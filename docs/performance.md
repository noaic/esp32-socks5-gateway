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

The current suite has 87 firmware/tool host regressions and 19 lwIP preparation regressions, plus frontend feedback/compatibility tests. Earlier hardware validation includes TCP echo, SOCKS TCP/HTTPS, DNS/ordinary UDP, two-client admission and isolated reconnect, configuration rollback, Web/forwarding coexistence, LED control, Internet checks and valid serial recovery. Those board runs predate the audit fixes described below.

Complete QUIC compatibility, real upstream interruptions, mid-session UDP control failure, long-lived cached DHCP leases, long-term stability and Flash-write power loss still need dedicated coverage. A short successful benchmark is not evidence that all these cases pass.

Raw local logs and binaries are excluded from the public repository. Sanitized historical notes remain under `history/`. The publication copy uses example addresses, empty STA defaults and a portable UART test interpreter; its clean build/host regression are validated separately and are not flashed over the existing board's NVS configuration.

## Local audit fixes and verification (2026-10-14)

The pinned ESP-IDF/esp-lwIP build and complete override check pass on Linux. The resulting application `.bin` is 979840 bytes (approximately 957 KiB); `idf.py size` reports 979722 bytes before image padding. Its 4 MiB application partition retains about 77% free space. Compared with the 975584-byte audit baseline built with the same configuration, the binary grows by 4256 bytes. Static data is 23015 bytes (+48) and bss remains 27928 bytes. The mapping table adds 16 bytes of counters in PSRAM; no FreeRTOS task is added. These are build measurements, not board heap/stack samples.

All 106 host tests pass without skips. Coverage includes HTTP close behavior and deadlines over real host sockets (including lwIP's sub-millisecond timeout boundary), per-client retained/pending TCP mapping limits, UDP payload-byte accounting, committed SDK source verification, and serial echo/prompt via a pseudo-terminal and mocked UART driver. Frontend tests and headless Chromium cover English/Chinese events and unsaved Internet-check choices; capture tests check prompt display without a newline. HTTP tests shorten the deadline for practical host execution; they do not emulate the Wi-Fi driver or real-board scheduling.

TCP metadata is now limited to 256 retained records and 16 unclaimed initial records per client slot, even with one client. The shared 64 active TCP / 32 UDP limits and 120-second closing-record guard are unchanged. This protects mapping space but reduces how many rapid initial/short connections a single slot can retain. No new radio throughput measurement establishes its impact; the historical speed table remains a reference, not a benchmark of these fixes.

Windows/WSL2 installation and real-board flash, UART, slow HTTP, SYN pressure, heap/stack and radio performance have not been rerun for this local revision. They remain hardware/platform checks before claiming a new performance or stability result.
