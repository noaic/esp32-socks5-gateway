# Performance, resources and validation

## Short-term measurements

| Scenario | Download Mbps | Upload Mbps |
| --- | ---: | ---: |
| serial50, one client / one stream | 16.43 | 16.25 |
| serial50, one client / four-stream aggregate | 24.49 | 21.25 |
| web51, A idle / B one stream | 14.21 | 11.97 |
| web51, A playing 1080P / B one stream | 14.41 | 11.83 |

These are historical board measurements, not benchmarks of every later commit or Release. Android clients used a LAN test service, with three completed rounds per scenario and separate download/upload measurements. Only the last two rows form a same-session mixed-load comparison: approximately ±1% difference, with no reported stalls on A. Video can buffer and its actual bitrate was unknown; this does not prove fairness under sustained saturation. Different historical phones/background conditions also prevent attributing a throughput difference to management features.

The records are [serial50](history/serial50.md#测速回归2026-10-05) and [web51 mixed load](history/mixed-load51.md). Raw local logs and binaries are excluded from the repository; the sanitized notes preserve reported results but do not permit independent recalculation from all original samples.

ESP32-S3 has a single 2.4 GHz radio shared by AP and STA. Two clients do not double throughput. Wi-Fi/lwIP still need internal RAM despite ample PSRAM. Changes to buffers, queue limits, diagnostics or validation require a new board benchmark before claiming a speed gain or absence of regression.

## Connection limits

| Resource / policy | Current behavior |
| --- | --- |
| Total forwarding capacity | 64 active TCP relays and 32 UDP flows, shared by both clients |
| TCP mapping records | 512 total; at most 256 retained records and 16 initial records awaiting listener acceptance per client slot, including earlier connection generations of that slot |
| Closing TCP records | Kept for at least 120 seconds (2×MSL); rapid short connections can exhaust mappings before all active relay slots are occupied |
| TCP upstream connection / SOCKS negotiation | 15-second deadline |
| TCP inactivity | Close after 120 seconds; at capacity, an empty-queue relay with a previous response and at least 30 seconds idle can be reclaimed |
| UDP payload | At most 1462 bytes per datagram; no IPv4 fragments/options or SOCKS UDP fragments |

The per-slot mapping limits also apply with only one client. When two clients are associated, new admission is limited to 48 TCP and 24 UDP flows per client; already-established flows are not evicted when the second client joins. Some UDP capacity is reserved for DNS. These are resource/admission policies, not equal bandwidth allocation. See [architecture](architecture.md#forwarding) and `main/proxy_config.h` for implementation details.

## Released baseline and verification

The published baseline is [v26.10.16](https://github.com/noaic/esp32-socks5-gateway/releases/tag/v26.10.16), from clean source `0be5693`. The following results describe that baseline; later source changes need their own verification.

| Check | Recorded result | Boundary |
| --- | --- | --- |
| Pinned SDK/lwIP, build and size | Linux x86_64 passed; app BIN 995024 bytes, `idf.py size` 994902 bytes; data/bss 23015/27928 bytes | Build measurements, not runtime heap/stack or radio samples |
| Host regressions | 125 passed without skips | C/Python behavior with host sockets, fixtures and mocked ESP APIs |
| Frontend checks | 434 Node assertions, four page/language transport combinations, 416 Chromium assertions | English/Chinese desktop/mobile pages with mocked APIs |
| GitHub Actions | [Run 38868348321](https://github.com/noaic/esp32-socks5-gateway/actions/runs/38868348321) succeeded on `34e9770`; the release commit adds documentation only | Build/test/artifact automation; it does not publish a Release |
| Firmware assets | Two versioned `-factory.bin` / `-sysupgrade.bin` downloads; merged factory compared byte-for-byte with official esptool; download SHA-256 verified | No physical flashing or boot verification in this environment |

The 4 MiB application partition had about 76% free space. The released factory image was 1060560 bytes at `0x0`; the application-only sysupgrade image was 995024 bytes at `0x10000`. Factory padding includes NVS and clears saved settings; compatible app-only updates normally preserve NVS. See [flashing and compatibility](flash-prebuilt.md). Historical local audit/build deltas are preserved in [the dated record](history/audit-validation-2026-10.md).

Run the [current regression commands](development.md#run-the-existing-regression-tests) for the source you actually build; test counts can change. Host tests cover protocol parsing, bounded forwarding, configuration/recovery, HTTP transport, serial input, lwIP preparation and firmware packaging. They do not emulate the Wi-Fi driver or real-board scheduling.

Detailed UART diagnostics are disabled by default. `CONFIG_PROXY_VERBOSE_DIAGNOSTICS` enables detailed RX-buffer/reference tracking, Wi-Fi TX-completion observation, per-packet timing and CPU runtime samples. Basic Web status/error counters remain available without it. Existing `sdkconfig` files retain their saved choices; use a new configuration or review runtime-statistics choices in `menuconfig`. Disabling these measurements reduces instrumentation in the data path; no new radio result quantifies its effect.

## Unreleased local audit validation (2026-10-18)

Two independent builds with fresh configurations and cold build directories passed against the pinned SDK/lwIP. These results were recorded for local changes after `v26.10.16`, before a new Release was published; they do not describe the released `v26.10.16` binary.

| Configuration | Application BIN bytes | `idf.py size` bytes | Static data / bss bytes |
| --- | ---: | ---: | ---: |
| Default, detailed diagnostics off | 995872 | 995750 | 22967 / 27352 |
| `CONFIG_PROXY_VERBOSE_DIAGNOSTICS` enabled | 1009040 | 1008922 | 23015 / 30744 |

Fresh defaults disable FreeRTOS runtime statistics; the detailed configuration enables them with ESP_TIMER and a 64-bit counter. ELF inspection confirmed the four detailed RX/reference wrappers are absent from the default image and present in the diagnostic image. Node passed 500 assertions plus the four transport combinations; Chromium passed 596 assertions across English/Chinese desktop and mobile pages. These are local build/frontend checks, not runtime board heap/stack or radio measurements.

All 138 host regressions passed without skips. The mapping-table pressure case uses simulated time: 400 distinct short-connection attempts over 100 seconds admitted 256 and rejected 144, while the other client slot remained usable and admission recovered after the closing guard expired. This documents the unchanged mapping policy, not end-to-end connection throughput or a new radio measurement.

Full-package checksums, ZIP extraction and hardware-free helper checks passed. Official esptool 5.6.0 produced a byte-identical merged factory image and verified the application checksum/hash. Formal Release export rejected the actual dirty build before creating either output directory; clean-tag export and failure cleanup are covered by host fixtures. No physical flashing or new Release publication was performed.

## Historical runtime resource samples

The tested local web51 application was approximately 945344 bytes. At AP+STA startup, free internal heap/PSRAM were approximately 91231/6984320 bytes. A 16 MB Flash chip does not make all Flash available to the application, and total internal RAM is not the allocatable heap.

Mixed-load samples showed 76051 and 77751 bytes of free internal heap, with a largest block of 31744 bytes. The since-boot minimum of 25280 bytes had already occurred before that test. Maintenance-worker minimum free stack was 1032 out of 6144 bytes. These sampled/historical values do not capture every transient peak.

After network recovery, internal free/minimum/largest-block values were 79307/41620/34816 bytes, PSRAM free was 6962576 bytes, and maintenance-stack minimum free was 1084/6144 bytes. Minima across different boots are not direct evidence of a leak or performance change.

## Hardware coverage and remaining limits

Earlier board validation included TCP echo, SOCKS TCP/HTTPS, DNS/ordinary UDP, two-client admission and isolated reconnect, configuration rollback, Web/forwarding coexistence, LED control, Internet checks and valid serial recovery. These board runs predate the audit and released binary; they are historical coverage, not validation of each subsequent change.

Windows/WSL2 installation and physical flashing, current UART/slow-HTTP/SYN-pressure behavior, heap/stack peaks and radio throughput have not been rerun for this source revision. Complete QUIC compatibility, client NTP traffic, real upstream interruptions, mid-session UDP control failure, long-lived cached DHCP leases, long-term stability and Flash-write power loss still need dedicated coverage. A short successful benchmark or a passing cloud build cannot establish these results.
