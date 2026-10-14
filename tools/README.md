# Tool usage

Run commands from the project root. Use the activated ESP-IDF Python environment for lwIP preparation and UART capture; it includes pyserial/esptool. Other Python tools use the standard library, except the optional QUIC probe, which needs `aioquic`. Firmware does not run these host tools.

Replace the example LAN addresses, ports and serial ports with yours. The commands below show test setup, not evidence of working services at those addresses. Use only one process per serial port. Linux/WSL2 paths and PowerShell paths are shown separately where they differ.

## Prepare the lwIP override

```text
python tools/prepare_lwip.py apply
python tools/prepare_lwip.py check
```

`apply` verifies the pinned SDK and source contents, copies its lwIP component into ignored `components/lwip`, and patches local `ip4.c`. `check` repeats verification without writing. `restore` reverses only the verified local `ip4.c` patch, without needing an SDK environment:

```text
python tools/prepare_lwip.py restore
```

Restore is not a full component integrity check. Run `apply`/`check` again before building the default transparent mode. An explicit `--idf-path` may replace `IDF_PATH`. See the [build guide](../README.md#install-the-toolchain-and-build) for exact revisions and LF/CRLF handling; the tool never patches the global SDK.

## Serial capture

Linux/WSL2, after attaching the USB device:

```bash
python tools/uart_capture.py --port /dev/ttyACM0 --no-reset --timestamps --seconds 30 --log capture.log
```

Windows PowerShell:

```powershell
python tools/uart_capture.py --port COM5 --no-reset --timestamps --seconds 30 --log capture.log
```

Capture uses 115200 baud. `--seconds 0` runs until interrupted; `--timestamps` adds host receive times to log lines and displays received prompts/echo immediately. It does not assign ESP32 timestamps. `--no-reset` avoids the tool's active reset; opening the adapter can still affect reset lines. Without this option the tool actively resets the board.

On Linux/WSL2, add `--interactive` to send ASCII lines from stdin after Enter. This is not a character-by-character line editor; the host terminal may echo the line too. Windows stdin is unsupported by this option: use `idf.py -p COM5 monitor` or a serial terminal for commands. Firmware echoes input and displays `gateway> `; disable local echo in a terminal that sends each character. `--info-logs` automatically sends `log info` on attachment/after a Ready banner and increases diagnostics; return to `log warn` afterward. `uart_passive.py` is an internal POSIX helper, not a separate command-line tool.

## Probe the upstream service directly

These probes run **from the computer to SOCKS5**, bypassing the ESP32. They help distinguish upstream UDP/firewall failures from gateway forwarding failures. Success does not verify the ESP32 path.

DNS through SOCKS5 UDP (standard-library Python; Linux and Windows):

```text
python tools/probe_upstream_udp.py --host 192.168.1.2 --port 7897 --dns 1.1.1.1 --name example.com
```

The proxy must accept IPv4 NO AUTH and UDP ASSOCIATE; its returned UDP endpoint must be reachable. A timeout may also come from the selected DNS service or network policy.

The optional `probe_upstream_quic.py` checks a QUIC TLS handshake, not full HTTP/3 traffic. Install `aioquic` in a separate host environment to keep IDF build dependencies intact. Linux example:

```bash
python3 -m venv /tmp/esp32-quic-probe
/tmp/esp32-quic-probe/bin/python -m pip install aioquic
/tmp/esp32-quic-probe/bin/python tools/probe_upstream_quic.py --host 192.168.1.2 --port 7897 --ip 1.1.1.1 --name cloudflare-dns.com
```

PowerShell example:

```powershell
python -m venv "$env:TEMP\esp32-quic-probe"
& "$env:TEMP\esp32-quic-probe\Scripts\python.exe" -m pip install aioquic
& "$env:TEMP\esp32-quic-probe\Scripts\python.exe" tools/probe_upstream_quic.py --host 192.168.1.2 --port 7897 --ip 1.1.1.1 --name cloudflare-dns.com
```

`--ip` and `--name` must identify the same QUIC service; certificate validation remains enabled. This probe cannot establish complete gateway QUIC compatibility.

## LAN throughput service

Run on a LAN computer reachable through the upstream proxy. Bind to that computer's LAN address and allow TCP 18080 on its trusted LAN firewall. Linux:

```bash
python tools/test_stream_server.py --bind 192.168.1.10 --port 18080 --label local-test --results /tmp/esp32-bench-results.jsonl
```

PowerShell uses a Windows results path instead of the script's `/tmp` default:

```powershell
python tools/test_stream_server.py --bind 192.168.1.10 --port 18080 --label local-test --results "$env:TEMP\esp32-bench-results.jsonl"
```

On a phone/computer connected to the ESP32 AP, open `http://192.168.1.10:18080/bench`. This serves `throughput_bench.html`; do not open the HTML as a local file. Completed reports are appended to the specified JSONL file and printed by the service. The output directory must already exist. Start with one client/one stream, then compare concurrent streams under the same conditions. The test service is unauthenticated; stop it after testing and keep it on a trusted LAN.

This host service is separate from the optional on-board diagnostic benchmark. Do not enable board diagnostics merely to run this external browser test.

## Transparent TCP echo

Run on a computer connected to the ESP32 AP. The default `198.51.100.7:443` is a documentation/test endpoint used by the optional local echo mode; it is not a public echo service. That mode requires disabling `Forward TCP through SOCKS5 (disable for Phase 2 echo)` in menuconfig and matching the configured test destination. In normal SOCKS forwarding mode, use a dedicated plain TCP echo server reachable through the proxy. Sending arbitrary echo bytes to a normal HTTPS server will fail.

Linux/Python, with an example dedicated echo target:

```bash
python tools/test_transparent_echo.py --client-ip 192.168.4.2 --remote 192.168.1.10 --port 19000 --rounds 64
```

This Python script checks that the source belongs to the default `192.168.4.0/24`, even if `--client-ip` is supplied. It cannot test a changed AP subnet without adapting that test. The source address must belong to the AP client's own interface.

Windows PowerShell:

```powershell
.\tools\test-transparent-echo.ps1 -ClientIP 192.168.4.2 -RemoteIP 192.168.1.10 -Port 19000 -Rounds 64
```

Omit `-ClientIP` only when exactly one default-subnet AP interface can be auto-detected. For a changed AP subnet, provide its actual client address explicitly. The tests compare payloads, check the original remote endpoint, and exercise FIN/EOF; they are not throughput benchmarks.

## Reports and local file inventories

`collect_link_bench.py` reads an existing timestamped UART log without opening the serial port:

```text
python tools/collect_link_bench.py capture.log --output link-results.jsonl
```

It extracts only the supported historical `diag35-ap-local` / `diag36-sta-socks` `link_bench: BENCH_RESULT` reports. The corresponding on-board test requires `CONFIG_PROXY_LINK_BENCH`, disabled by default. Ordinary serial logs or the external test service's JSONL results will not produce these records. A zero-record result is not a successful speed test.

First copy the desired build/configuration files into a separate, nonempty local snapshot directory, then run:

```text
python tools/snapshot_manifest.py create local-snapshot
python tools/snapshot_manifest.py check local-snapshot
```

`create` records names, sizes and SHA-256 hashes in `manifest.json`, refusing to replace an existing manifest. `check` detects missing, extra or changed files. It does not create the snapshot, back up board NVS, or flash anything. Configuration files and logs may contain credentials; keep such snapshots private.
