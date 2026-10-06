#pragma once
#include <stdint.h>
#include "esp_netif.h"

/* Diagnostic only: nonblocking request, IPv4 address in network byte order. */
void link_probe_init(esp_netif_t *ap, esp_netif_t *sta);
void link_probe_request(uint32_t client_ip);
