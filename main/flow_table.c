#include "flow_table.h"
#include <string.h>
_Static_assert(PROXY_MAX_TCP_MAPPINGS < 32768, "int16 index range");
_Static_assert(PROXY_FLOW_PORT_BASE + PROXY_MAX_TCP_MAPPINGS <= 65536, "port range");
static unsigned bucket(uint32_t client, uint16_t cp, uint32_t remote, uint16_t rp)
{
    uint32_t x = client ^ (remote * 2654435761u) ^ ((uint32_t)cp << 16) ^ rp;
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15;
    return x & (PROXY_FLOW_BUCKETS - 1);
}
void flow_table_init(flow_table_t *t)
{
    memset(t, 0, sizeof(*t));
    for (unsigned i=0; i<PROXY_FLOW_BUCKETS; i++) t->heads[i]=-1;
}
int flow_table_input(flow_table_t *t, const proxy_tcp_packet_t *p,
                     int64_t now, flow_t *snapshot, bool *created)
{
    return flow_table_input_owned(t,p,0,now,snapshot,created);
}
int flow_table_input_owned(flow_table_t *t, const proxy_tcp_packet_t *p,
                           uint32_t token,int64_t now,flow_t *snapshot,bool *created)
{
    *created=false;
    unsigned b=bucket(p->src_ip,p->src_port,p->dst_ip,p->dst_port);
    int found=t->heads[b];
    while (found>=0) {
        flow_t *f=&t->flows[found];
        if (f->client_ip==p->src_ip && f->client_port==p->src_port &&
            f->remote_ip==p->dst_ip && f->remote_port==p->dst_port && f->client_token==token) break;
        found=f->next;
    }
    if (found<0 && (p->flags & 0x17)==0x02 && t->used<PROXY_MAX_TCP_MAPPINGS) {
        for (unsigned n=0; n<PROXY_MAX_TCP_MAPPINGS; n++) {
            unsigned i=(t->cursor+n)%PROXY_MAX_TCP_MAPPINGS;
            if (!t->flows[i].used) { found=i; t->cursor=(i+1)%PROXY_MAX_TCP_MAPPINGS; break; }
        }
        if (found>=0) {
            t->flows[found]=(flow_t){.used=true,.client_ip=p->src_ip,.remote_ip=p->dst_ip,.client_token=token,
                .client_port=p->src_port,.remote_port=p->dst_port,
                .local_peer_port=PROXY_FLOW_PORT_BASE+found,.next=t->heads[b]};
            t->heads[b]=found; t->used++; *created=true;
        }
    }
    if (found>=0) {
        flow_t *f=&t->flows[found]; f->touched_us=now; f->input_packets++;
        *snapshot=*f; f->input_logged |= p->flags & 0x05;
    }
    return found;
}
int flow_table_peer(flow_table_t *t, uint32_t ip, uint16_t port)
{
    int i=(int)port-PROXY_FLOW_PORT_BASE;
    return i>=0 && i<PROXY_MAX_TCP_MAPPINGS && t->flows[i].used &&
           t->flows[i].client_ip==ip ? i : -1;
}
unsigned flow_table_expire(flow_table_t *t, int64_t now, int64_t guard)
{
    unsigned expired=0;
    for (unsigned i=0; i<PROXY_MAX_TCP_MAPPINGS; i++) {
        flow_t *f=&t->flows[i];
        if (!f->used || f->socket_open || now-f->touched_us<guard ||
            (f->retired && now-f->retired_us<guard)) continue;
        unsigned b=bucket(f->client_ip,f->client_port,f->remote_ip,f->remote_port);
        int16_t *link=&t->heads[b];
        while (*link>=0 && *link!=(int)i) link=&t->flows[*link].next;
        if (*link>=0) *link=f->next;
        memset(f,0,sizeof(*f)); t->used--; expired++;
    }
    return expired;
}
