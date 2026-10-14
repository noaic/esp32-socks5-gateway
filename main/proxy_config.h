#pragma once

/* IPv4 NO AUTH upstream. Echo rollback mode does not connect here. */
#ifdef ESP_PLATFORM
#include "gateway_settings.h"
#define PROXY_SOCKS5_IPV4 (gateway_settings()->socks_ipv4)
#define PROXY_SOCKS5_PORT (gateway_settings()->socks_port)
#else
#define PROXY_SOCKS5_IPV4 "192.168.1.2"
#define PROXY_SOCKS5_PORT 7897
#endif
#define PROXY_ECHO_PORT 15000
#define PROXY_AP_MAX_CLIENTS 2
/* Admission only when both clients are associated. Never evict live flows
 * when a second client joins; 64 TCP / 32 UDP remain shared global limits. */
#define PROXY_CLIENT_TCP_LIMIT 48
#define PROXY_CLIENT_UDP_LIMIT 24
/* Includes closed/TIME_WAIT mappings; live relay limit remains 64. */
#define PROXY_MAX_TCP_MAPPINGS 512
#define PROXY_CLIENT_TCP_MAPPINGS 256 /* Includes retained generations of one client slot. */
#define PROXY_CLIENT_TCP_PENDING 16 /* Initial SYN mappings not claimed by the listener. */
#define PROXY_ECHO_IDLE_SECONDS 60

#define PROXY_MAX_UDP_FLOWS 32
#define PROXY_UDP_DNS_RESERVED 4 /* Preserve DNS admission during UDP retry bursts. */
#define PROXY_UDP_MAX_PAYLOAD 1462 /* 1500 - IPv4(20) - UDP(8) - SOCKS UDP(10). */
#define PROXY_UDP_IDLE_SECONDS 60
#define PROXY_UDP_DNS_IDLE_SECONDS 30 /* Completed flows remain reclaimable at capacity. */
#define PROXY_UDP_DNS_PRESSURE_SECONDS 10 /* Full-table deadline for tracked unanswered queries. */
#ifdef ESP_PLATFORM
#define PROXY_DNS_IPV4 (gateway_settings()->dns_ipv4)
#else
#define PROXY_DNS_IPV4 "1.1.1.1"
#endif
