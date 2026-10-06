#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "packet_rewrite.h"
#include "proxy_config.h"

/* Addresses are network order, ports host order. Opaque AP pointer is only
 * interpreted by the TCPIP adapter. Queued data always owns its bytes. */
typedef struct {
    proxy_udp_packet_t tuple;
    void *ap;
    uint32_t generation;
    uint32_t client_token;
    int64_t queued_us;
    uint8_t bytes[PROXY_UDP_MAX_PAYLOAD];
} udp_datagram_t;

typedef enum {
    UDP_RX, UDP_RX_DNS, UDP_RX_443, UDP_TX, UDP_TX_DNS, UDP_TX_443,
    UDP_REPLY, UDP_REPLY_DNS, UDP_REPLY_443, UDP_DELIVERED,
    UDP_TOO_BIG, UDP_BAD_INPUT, UDP_NOT_READY, UDP_INPUT_FULL, UDP_STALE,
    UDP_ASSOC_LOST, UDP_FLOW_FULL, UDP_FLOW_SOCKET, UDP_SEND_DROP,
    UDP_RECEIVE_DROP, UDP_BAD_REPLY, UDP_DELIVERY_FULL, UDP_CALLBACK_FULL,
    UDP_OUTPUT_DROP, UDP_DNS_RECLAIMED, UDP_NON_DNS_LIMIT,
    UDP_PENDING_FULL, UDP_PENDING_EXPIRED,
    UDP_INPUT_FULL_DNS, UDP_INPUT_FULL_ORDINARY,
    UDP_OUTPUT_ALLOC, UDP_OUTPUT_MEM, UDP_OUTPUT_BUF, UDP_OUTPUT_IF,
    UDP_OUTPUT_DOWN, UDP_OUTPUT_OTHER, UDP_OUTPUT_RETRY,
    UDP_OUTPUT_RECOVERED, UDP_DELIVERY_EXPIRED, UDP_DELIVERY_EXHAUSTED,
    UDP_DNS_PRESSURE_RECLAIMED,
    UDP_DIAG_COUNT
} udp_diag_t;

typedef struct {
    unsigned active, ready, handshake;
    uint64_t dispatches, queue_wait_total_us, queue_wait_max_us;
    uint64_t dns_dispatches, dns_wait_total_us, dns_wait_max_us;
    uint64_t warm_dispatches, warm_wait_total_us, warm_wait_max_us;
    uint64_t associations, association_total_us, association_max_us;
    uint64_t dns_replies, dns_rtt_total_ms, dns_rtt_max_ms;
} udp_relay_stats_t;

typedef struct {
    bool (*take)(void *context, bool dns, udp_datagram_t *message);
    bool (*pending)(void *context);
    void (*snapshot)(void *context, bool *online, uint32_t *generation);
    bool (*reply)(void *context, const udp_datagram_t *message,
                  const uint8_t *data, size_t size);
    void (*count)(void *context, udp_diag_t counter, unsigned amount);
    void (*stats)(void *context, const udp_relay_stats_t *stats);
    void (*association_error)(void *context,int code); /* Optional bounded health observer. */
    /* Optional adapter maintenance; returns microseconds until next deadline.
     * The worker uses this to retry local output without another task/timer. */
    int64_t (*maintain)(void *context);
    bool (*stop)(void *context); /* NULL in firmware; bounded host harness. */
    /* Optional per-client lifecycle. Called only by the single worker. */
    bool (*client_current)(void *context,uint32_t token);
    bool (*client_acquire)(void *context,uint32_t token,bool dns);
    void (*client_release)(void *context,uint32_t token,bool dns);
    void (*client_traffic)(void *context,uint32_t token,bool upload);
} udp_relay_callbacks_t;

/* Allocate fixed flow storage once (PSRAM on ESP32); one worker owns sockets. */
bool udp_relay_init(void);
void udp_relay_run(const char *upstream, uint16_t port, int wake_fd,
                   const udp_relay_callbacks_t *callbacks, void *context);
