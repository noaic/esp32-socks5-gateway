#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "packet_rewrite.h"
#include "proxy_config.h"
#define PROXY_FLOW_BUCKETS 128
#define PROXY_FLOW_PORT_BASE 20000
/* Caller serializes access. Metadata is independent of lwIP PCB/TCP state. */
typedef struct {
    bool used, socket_open, retired;
    uint8_t input_logged, output_logged;
    uint32_t client_ip, remote_ip, client_token;
    uint16_t client_port, remote_port, local_peer_port;
    int16_t next;
    uint32_t input_packets, output_packets;
    int64_t touched_us, retired_us;
} flow_t;
typedef struct {
    flow_t flows[PROXY_MAX_TCP_MAPPINGS];
    int16_t heads[PROXY_FLOW_BUCKETS];
    unsigned used, cursor;
    unsigned owned[PROXY_AP_MAX_CLIENTS], pending[PROXY_AP_MAX_CLIENTS];
} flow_table_t;
void flow_table_init(flow_table_t *table);
int flow_table_input(flow_table_t *table, const proxy_tcp_packet_t *packet,
                     int64_t now, flow_t *snapshot, bool *created);
int flow_table_input_owned(flow_table_t *table, const proxy_tcp_packet_t *packet,
                           uint32_t token, int64_t now, flow_t *snapshot, bool *created);
int flow_table_peer(flow_table_t *table, uint32_t client_ip, uint16_t local_peer_port);
bool flow_table_claim(flow_table_t *table,int index,flow_t *snapshot);
void flow_table_retire(flow_table_t *table,int index,int64_t now);
unsigned flow_table_expire(flow_table_t *table, int64_t now, int64_t guard);
