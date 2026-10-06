#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "proxy_config.h"

/* A token identifies one association and DHCP address, never just an IP.
 * All APIs serialize internally; workers remain owners of socket closes. */
typedef struct {
    bool connected;
    uint8_t mac[6];
    uint32_t ip, token;
    unsigned tcp, udp, ordinary_udp, tcp_denied, udp_denied;
    uint64_t tcp_up, tcp_down, udp_up, udp_down;
} proxy_client_info_t;
uint32_t proxy_client_connect(const uint8_t mac[6]);
bool proxy_client_assign(const uint8_t mac[6], uint32_t ip);
void proxy_client_disconnect(const uint8_t mac[6]);
void proxy_clients_reset(void);
uint32_t proxy_client_token(uint32_t ip);
bool proxy_client_current(uint32_t token);
bool proxy_client_acquire(uint32_t token, bool udp, bool dns);
void proxy_client_release(uint32_t token, bool udp, bool dns);
void proxy_client_traffic(uint32_t token, bool udp, bool upload, uint64_t amount);
void proxy_clients_snapshot(proxy_client_info_t out[PROXY_AP_MAX_CLIENTS]);
