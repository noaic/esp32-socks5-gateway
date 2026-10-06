# Publication baseline

Prepared on 2026-10-07 from the locally validated web51 project. The public project/CMake name is `esp32-socks5-gateway`; the original development workspace and board firmware were left unchanged.

Publication changes:

- Empty STA credentials in generated clean configuration; no generated sdkconfig, NVS, personal logs or binaries tracked.
- Local network addresses, device MACs and user paths replaced with examples. Default upstream is `192.168.1.2:7897`; optional diagnostic peer is `192.168.1.10:18080`.
- Test fixtures updated with their corresponding example addresses, including the subnet-overlap rejection and binary SOCKS CONNECT request.
- UART test uses the invoking Python interpreter instead of an author-specific absolute environment path.
- English primary README, Chinese and Japanese READMEs, current architecture/maintenance/performance guides, and sanitized development history.

Validation of this publication copy:

- Clean ESP-IDF v6.1 build/size passed. Application binary: 945280 bytes; built with empty STA SSID/password. No automatic flash was performed.
- All 75 Linux host regression tests passed, none skipped.
- 23 feedback assertions, 11 complete-page assertions and fetch/XHR transport compatibility tests passed. The JavaScript checks were executed in the available V8 runtime; Node CLI runners are included.
- The pinned project-local lwIP check passed; global ESP-IDF remained untouched.
- Primary documentation links and the staged-file credential/exclusion checks passed.

Hardware observations describe the original tested board baseline; changing addresses and build defaults has separate build/host validation and is not claimed to be newly flashed hardware verification. Source archives contain tracked repository files only. Regenerated firmware hashes depend on timestamps and build configuration.
