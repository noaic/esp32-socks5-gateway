#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
esp_err_t udp_gateway_start(void);
void udp_gateway_set_sta_ready(bool ready);
void udp_gateway_reset_client(void);
/* Runs in TCPIP context; input pbuf ownership stays with IPv4 caller. */
err_t udp_gateway_input(struct pbuf *packet, struct netif *inp);
