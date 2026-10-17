#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

/* Bind before Wi-Fi starts; handles live for the lifetime of the application. */
void net_diagnostics_init(esp_netif_t *ap,esp_netif_t *sta);
/* With verbose diagnostics, install the pinned v6.1 TX completion observer
 * after esp_wifi_start(). Otherwise this is a successful no-op. */
esp_err_t net_diagnostics_start_wifi(void);
void net_diagnostics_log(void);
uint32_t net_diagnostics_ap_udp_attempts(void);
/* Called in the serialized TCPIP output callback only, after ERR_MEM. */
void net_diagnostics_udp_mem(uint32_t before,bool arp_resolved);
void net_diagnostics_clone_failed(bool output);
/* Read-only atomic counters; no PCB walk or TCPIP callback required. */
typedef struct {
    uint32_t tcp_pcb_live, tcp_pcb_peak, tcp_pcb_failures;
    uint32_t ap_tx_errors, sta_tx_errors, clone_failures;
    uint32_t client_rto, upstream_rto, client_fast, upstream_fast;
} net_diagnostics_snapshot_t;
void net_diagnostics_snapshot(net_diagnostics_snapshot_t *out);
