# Code architecture

`main/app_main.c` initializes configuration/NVS, AP+STA, DHCP and network events, then starts the fixed forwarding and management workers. Network epochs and MAC/IP/session tokens identify flow ownership. One client leaving does not reset the other client's complete forwarding state.

## Forwarding

`tcp_poc.c`, `packet_rewrite.c` and `flow_table.c` connect the project-owned IPv4 rewrite hooks to lwIP. `socks_relay.c` implements nonblocking SOCKS negotiation, bounded bidirectional queues, EOF handling and timeouts. TCP correctness remains lwIP's responsibility.

The transparent TCP listener binds to the AP IPv4 address. The 512 mapping records have per-client-slot limits of 256 retained records and 16 initial records awaiting listener acceptance, including earlier token generations of that slot. Counters are maintained under the existing flow lock without another per-packet scan. Live records are not evicted, and closing records retain the existing 2×MSL guard (120 seconds); short-connection bursts can exhaust a slot's quota. Upstream negotiation is bounded to 15 seconds, idle relays to 120 seconds, and full-capacity admission may reclaim an empty-queue relay idle for 30 seconds.

`udp_gateway.c`, `udp_relay.c`, `socks_udp.c` and `dns_flow.c` handle interception, SOCKS associations, protocol headers, expiration and DNS-capacity protection. SOCKS UDP fragmentation, broadcast/multicast and special P2P semantics are outside V1.

Transparent parsing rejects IPv4 options/fragments; UDP payloads are bounded to 1462 bytes so IPv4, UDP and the 10-byte SOCKS header fit a 1500-byte upstream packet. This is separate from rejecting SOCKS `FRAG != 0`.

`ap_clients.c` holds two static client records, tokens, admission counts and traffic counters. Total forwarding capacity remains fixed. With two associated clients, new admission is limited to 48 TCP and 24 UDP flows per client; existing flows are not forcibly evicted when a second client joins. Admission limits are not strict bandwidth fairness or reserved instantaneous capacity.

DHCP assignment registers a client's IPv4 identity; static-IP clients are not supported for forwarding. Tokens distinguish reconnects and stale replies, not hostile IP spoofing. TCP/UDP counters count relayed payload bytes rather than mixing TCP bytes with UDP packet counts; they exclude protocol headers and cannot confirm physical/application delivery.

## Management

`gateway_settings.c`, `settings_validation.c` and `management_request.c` maintain the fixed versioned record, input validation, NVS serialization, first-use setup and 180-second confirmation/rollback. `web_management.c` and its embedded HTML pages expose AP-local management.

`management_http.c` installs socket receive/send overrides for the two HTTP sessions. A request has a 5-second overall network deadline, and the request wrapper rejects bodies over 1024 bytes. Handler errors or unread body bytes return failure so the SDK closes the session without draining leftover data. Clean requests reset the deadline for keep-alive reuse. The existing HTTP owner still performs configuration rollback; this bounds network stalls without introducing a new task or a second settings owner. It does not add TLS or make rollback timing a hard real-time guarantee.

`gateway_state.c` maintains status and bounded events. `gateway_control.c` uses the existing maintenance worker for diagnostics, reconnect/restart and serial recovery. `gateway_probe.c` performs SOCKS/DNS/HTTP checks. `serial_console.c`, `serial_commands.c` and `serial_recovery.c` implement English commands, bounded input/rate handling and one-use confirmation.

The serial parser echoes accepted ASCII, handles backspace, and redisplays `gateway> ` after a submitted line. CRLF submits once, rejected control bytes are not echoed, and the 95-byte input bound and one-command-per-second limit remain. Logs may interleave with input; this is a small command interface without ANSI/history editing.

Detailed CPU, TCP-window, local ping and AP benchmark diagnostics are optional and disabled in the everyday configuration. Low-frequency Internet checks reuse the maintenance worker; failed checks do not trigger automatic board restarts.

Management/first-use pages default to English and select Simplified Chinese only for a Chinese first browser language. Events expose stable `event_code` identifiers for frontend translation while retaining the legacy Chinese `event` field. Status polling preserves unsaved Internet-check preferences until a successful save or a changed boot identifier. Source investigation and experiment history, including reverted candidates, live in `history/`.

The management landing view shows connection overview, a suggested next action, and clients. Configuration, diagnostics, device details and maintenance use native disclosure controls; unconfigured networks, subnet conflicts and new failed checks open the relevant controls once. Closing a result is respected on later polls. Advice uses the existing staged probe results, distinguishes a check-target failure from general connectivity, and does not run disruptive actions automatically. The firmware version shown by the status API comes from the compiled ESP application description.

`tools/package_firmware.py` generates default N16R8 `factory.bin` (complete initial installation), `app.bin` (compatible application update), original split images, provenance, hashes and licenses, plus a ZIP containing no host scripts. Official esptool or Espressif's Windows GUI can flash the BIN directly. The complete raw image fills gaps with `0xFF`, including NVS, so it resets settings; app-only or compatible split-image updates normally preserve NVS. The optional `tools/flash_firmware.py` validates the package and writes its three original split images; it remains in the source checkout. These host tools do not change forwarding or the partition table. Read the [precompiled flashing guide](flash-prebuilt.md) or [source build guide](build-and-flash.md).
