# Code architecture

`main/app_main.c` initializes configuration/NVS, AP+STA, DHCP and network events, then starts the fixed forwarding and management workers. Network epochs and MAC/IP/session tokens identify flow ownership. One client leaving does not reset the other client's complete forwarding state.

## Forwarding

`tcp_poc.c`, `packet_rewrite.c` and `flow_table.c` connect the project-owned IPv4 rewrite hooks to lwIP. `socks_relay.c` implements nonblocking SOCKS negotiation, bounded bidirectional queues, EOF handling and timeouts. TCP correctness remains lwIP's responsibility.

`udp_gateway.c`, `udp_relay.c`, `socks_udp.c` and `dns_flow.c` handle interception, SOCKS associations, protocol headers, expiration and DNS-capacity protection. SOCKS UDP fragmentation, broadcast/multicast and special P2P semantics are outside V1.

`ap_clients.c` holds two static client records, tokens, admission counts and traffic counters. Total forwarding capacity remains fixed. With two associated clients, new admission is limited to 48 TCP and 24 UDP flows per client; existing flows are not forcibly evicted when a second client joins. Admission limits are not strict bandwidth fairness or reserved instantaneous capacity.

## Management

`gateway_settings.c`, `settings_validation.c` and `management_request.c` maintain the fixed versioned record, input validation, NVS serialization, first-use setup and 180-second confirmation/rollback. `web_management.c` and its embedded HTML pages expose AP-local management.

`gateway_state.c` maintains status and bounded events. `gateway_control.c` uses the existing maintenance worker for diagnostics, reconnect/restart and serial recovery. `gateway_probe.c` performs SOCKS/DNS/HTTP checks. `serial_console.c`, `serial_commands.c` and `serial_recovery.c` implement English commands, bounded input/rate handling and one-use confirmation.

Detailed CPU, TCP-window, local ping and AP benchmark diagnostics are optional and disabled in the everyday configuration. Low-frequency Internet checks reuse the maintenance worker; failed checks do not trigger automatic board restarts.

The Web interface currently uses Chinese; the translated READMEs document it without introducing firmware UI localization. Source investigation and experiment history, including reverted candidates, live in `history/`.
