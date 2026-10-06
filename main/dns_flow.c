#include "dns_flow.h"

static bool header(const uint8_t *data, size_t size, bool response)
{
    /* Standard QUERY opcode only. Unsupported traffic keeps its normal idle TTL. */
    return size>=12 && !!(data[2]&0x80)==response && !(data[2]&0x78);
}
static uint16_t id(const uint8_t *data) { return (uint16_t)data[0]<<8 | data[1]; }
void proxy_dns_sent(proxy_dns_pending_t *p, const uint8_t *data, size_t size)
{
    proxy_dns_sent_at(p,data,size,0);
}
void proxy_dns_sent_at(proxy_dns_pending_t *p, const uint8_t *data, size_t size, int64_t now)
{
    if (!header(data,size,false)) { p->untracked=true; return; }
    uint16_t transaction=id(data);
    for (unsigned i=0;i<p->count;i++) if (p->ids[i]==transaction) return;
    if (p->count==PROXY_DNS_PENDING_IDS) { p->untracked=true; return; }
    p->sent_ms[p->count]=now;
    p->ids[p->count++]=transaction;
}
void proxy_dns_replied(proxy_dns_pending_t *p, const uint8_t *data, size_t size)
{
    (void)proxy_dns_replied_at(p,data,size,0);
}
int64_t proxy_dns_replied_at(proxy_dns_pending_t *p, const uint8_t *data, size_t size, int64_t now)
{
    if (!header(data,size,true)) return -1;
    uint16_t transaction=id(data);
    for (unsigned i=0;i<p->count;i++) if (p->ids[i]==transaction) {
        int64_t rtt=now-p->sent_ms[i];
        p->ids[i]=p->ids[--p->count]; p->sent_ms[i]=p->sent_ms[p->count];
        p->replied=true; return rtt>=0?rtt:0;
    }
    return -1;
}
bool proxy_dns_reclaimable(const proxy_dns_pending_t *p)
{
    return p->replied && !p->untracked && p->count==0;
}
bool proxy_dns_pending_expired(const proxy_dns_pending_t *p,
                               int64_t now, int64_t timeout)
{
    if (p->untracked || !p->count || timeout<=0) return false;
    for (unsigned i=0;i<p->count;i++) {
        if (now<p->sent_ms[i] || now-p->sent_ms[i]<timeout) return false;
    }
    return true;
}
