#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifndef SOCKS_MAX_SESSIONS
#define SOCKS_MAX_SESSIONS 64
#endif
/* claim returns independent flow-table index and network-order original IPv4. */
typedef int (*socks_claim_fn)(int fd, uint32_t *ip, uint16_t *port);
typedef void (*socks_retire_fn)(int index);
typedef struct {
    uint32_t (*claim_owner)(int index);
    bool (*current)(uint32_t token);
    bool (*acquire)(uint32_t token);
    void (*release)(uint32_t token);
    void (*traffic)(uint32_t token,bool upload,unsigned bytes);
} socks_client_ops_t;
void socks_relay_run(int listener, const char *upstream, uint16_t port,
                     socks_claim_fn claim, socks_retire_fn retire);
void socks_relay_run_clients(int listener, const char *upstream, uint16_t port,
                            socks_claim_fn claim, socks_retire_fn retire,
                            const socks_client_ops_t *owners);

/* Thread-safe event notifications; worker owns all socket closes. */
void socks_relay_set_network_ready(bool ready);
void socks_relay_reset_client(void);

/* Allocate fixed metadata in internal RAM and payload buffers in PSRAM once. */
bool socks_relay_init(void);
