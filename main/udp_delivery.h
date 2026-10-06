#pragma once
#include "udp_relay.h"

#define UDP_DELIVERY_DEPTH 16
#define UDP_DELIVERY_DNS_RESERVED 4
#define UDP_DELIVERY_TTL_US 250000
#define UDP_DELIVERY_RETRY_US 20000
#define UDP_DELIVERY_MAX_ATTEMPTS 4

/* Only allocation/TX-memory/TX-buffer errors mean a locally rejected packet
 * that can be attempted again. Never retransmit after OK or a missing reply. */
typedef enum {
    UDP_EMIT_OK, UDP_EMIT_ALLOC, UDP_EMIT_MEM, UDP_EMIT_BUF,
    UDP_EMIT_IF, UDP_EMIT_DOWN, UDP_EMIT_OTHER, UDP_EMIT_STALE
} udp_emit_result_t;
typedef struct {
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    int64_t (*now_us)(void *ctx);
    bool (*current)(void *ctx,uint32_t generation);
    /* Success guarantees exactly one eventual callback; failure guarantees
     * that the callback was not queued. It may run before post() returns. */
    bool (*post)(void *ctx,void (*callback)(void *),void *arg);
    udp_emit_result_t (*emit)(void *ctx,const udp_datagram_t *message);
    void (*count)(void *ctx,udp_diag_t key,unsigned amount);
    void (*wake)(void *ctx);
} udp_delivery_ops_t;
typedef enum { UDP_SLOT_FREE, UDP_SLOT_FILLING, UDP_SLOT_WAIT,
               UDP_SLOT_POSTED, UDP_SLOT_RUNNING } udp_slot_state_t;
struct udp_delivery_pool;
typedef struct {
    struct udp_delivery_pool *pool;
    udp_slot_state_t state;
    unsigned attempts;
    int64_t accepted_us, due_us;
    udp_datagram_t message;
} udp_delivery_slot_t;
typedef struct udp_delivery_pool {
    udp_delivery_ops_t ops;
    void *ctx;
    unsigned cursor; /* One submit/pump worker; callbacks only own RUNNING. */
    udp_delivery_slot_t slots[UDP_DELIVERY_DEPTH];
} udp_delivery_pool_t;

void udp_delivery_init(udp_delivery_pool_t *pool,const udp_delivery_ops_t *ops,void *ctx);
bool udp_delivery_submit(udp_delivery_pool_t *pool,const udp_datagram_t *message,
                         const uint8_t *bytes,size_t length);
/* Bounded pump of up to four slots, DNS first with a two-slot ordinary budget.
 * POSTED/RUNNING remain reserved until their callback completes, even after
 * expiry/generation change; a stale callback must never see a reused slot. */
int64_t udp_delivery_pump(udp_delivery_pool_t *pool);
