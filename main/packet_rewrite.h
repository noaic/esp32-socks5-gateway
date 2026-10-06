#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t src_ip; /* network byte order, copied directly from IPv4 header */
    uint32_t dst_ip;
    uint16_t src_port; /* host byte order */
    uint16_t dst_port;
    uint8_t flags;
} proxy_tcp_packet_t;

/* Strict Phase 2 parser: IPv4/TCP, no options in IP header, no fragmentation.
 * Validates lengths plus original IPv4 and TCP pseudo-header checksums. */
bool proxy_tcp_parse(const uint8_t *data, size_t len, proxy_tcp_packet_t *packet);
/* Structural/IP checks only, for local lwIP output with software TCP checksum.
 * Reads just 40 bytes; must never replace strict parsing of untrusted input. */
bool proxy_tcp_parse_local_header(const uint8_t *header, size_t available,
                                  size_t packet_length, proxy_tcp_packet_t *packet);
bool proxy_tcp_rewrite(uint8_t *data, size_t len, bool rewrite_source,
                       uint32_t ip_network_order, uint16_t port_host_order);

typedef struct {
    uint32_t src_ip, dst_ip;
    uint16_t src_port, dst_port, payload_length;
} proxy_udp_packet_t;
/* IPv4 UDP: zero checksum is allowed; otherwise validate pseudo checksum. */
bool proxy_udp_parse(const uint8_t *data, size_t len, proxy_udp_packet_t *packet);

/* Also rewrite the other TCP port: internal source tag on input, real client port on output. */
bool proxy_tcp_rewrite_peer(uint8_t *data, size_t len, bool source,
                             uint32_t ip, uint16_t port, uint16_t peer_port);
/* Requires a private packet validated by the strict parser, or local lwIP output
 * checked by proxy_tcp_parse_local_header with software checksums enabled. */
void proxy_tcp_rewrite_validated(uint8_t *data, bool source,
                                 uint32_t ip, uint16_t port, uint16_t peer_port);
