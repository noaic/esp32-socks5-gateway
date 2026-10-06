#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"

esp_err_t tcp_poc_start(void);
/* Nonblocking request; PCB/ARP snapshot is read only in TCPIP context. */
void tcp_poc_trace_flow(int index, uint16_t upstream_local_port);
typedef struct {
    bool found;
    uint32_t snd_buf, snd_wnd, cwnd, flight, unsent, unacked, rcv_wnd;
} proxy_pcb_sample_t;
typedef struct {
    proxy_pcb_sample_t client, upstream;
    uint64_t callback_wait_us;
} proxy_pipeline_sample_t;
bool tcp_poc_pipeline_request(int index, uint16_t upstream_local_port);
bool tcp_poc_pipeline_poll(proxy_pipeline_sample_t *sample);
/* Invoked only from the lwIP TCPIP context by the project-owned patch.
 * Input: caller retains ownership on error; may replace/free *packet on success.
 * Output: returns 1 iff handled; never changes the original pbuf. */
err_t proxy_rewrite_ip4_input(struct pbuf **packet, struct netif *inp);
int proxy_rewrite_ip4_output(struct pbuf *packet, struct netif *out,
                             const ip4_addr_t *dest, err_t *result);
