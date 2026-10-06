#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROXY_DNS_PENDING_IDS 8
/* Observe transaction headers only; never change DNS bytes or resolve names. */
typedef struct {
    uint16_t ids[PROXY_DNS_PENDING_IDS];
    int64_t sent_ms[PROXY_DNS_PENDING_IDS];
    uint8_t count;
    bool untracked, replied;
} proxy_dns_pending_t;

void proxy_dns_sent(proxy_dns_pending_t *pending, const uint8_t *data, size_t size);
void proxy_dns_replied(proxy_dns_pending_t *pending, const uint8_t *data, size_t size);
bool proxy_dns_reclaimable(const proxy_dns_pending_t *pending);
/* Pressure-only timeout: all tracked pending IDs must have exhausted their
 * first-send deadline. Unknown/overflow traffic is never eligible. */
bool proxy_dns_pending_expired(const proxy_dns_pending_t *pending,
                               int64_t now_ms, int64_t timeout_ms);
void proxy_dns_sent_at(proxy_dns_pending_t *pending, const uint8_t *data,
                       size_t size, int64_t now_ms);
/* RTT from the first send of this ID; -1 for unmatched/unsupported replies. */
int64_t proxy_dns_replied_at(proxy_dns_pending_t *pending, const uint8_t *data,
                            size_t size, int64_t now_ms);
