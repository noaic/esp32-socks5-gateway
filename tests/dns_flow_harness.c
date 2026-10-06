#include "dns_flow.h"
#include <assert.h>
#include <stdio.h>

static void request(proxy_dns_pending_t *p, uint16_t id)
{
    uint8_t data[12]={id>>8,id,1,0,0,1};
    proxy_dns_sent(p,data,sizeof(data));
}
static void response(proxy_dns_pending_t *p, uint16_t id)
{
    uint8_t data[12]={id>>8,id,0x81,0x80,0,1};
    proxy_dns_replied(p,data,sizeof(data));
}
int main(void)
{
    proxy_dns_pending_t p={0};
    assert(!proxy_dns_reclaimable(&p));
    p=(proxy_dns_pending_t){0};
    uint8_t timed1[12]={0,1,1},timed2[12]={0,2,1};
    proxy_dns_sent_at(&p,timed1,12,100);
    proxy_dns_sent_at(&p,timed1,12,200); /* Retried ID keeps its original start. */
    proxy_dns_sent_at(&p,timed2,12,210);
    timed2[2]=0x81;
    assert(proxy_dns_replied_at(&p,timed2,12,240)==30);
    timed1[2]=0x81;
    assert(proxy_dns_replied_at(&p,timed1,12,260)==160);
    assert(proxy_dns_replied_at(&p,timed1,12,270)==-1);
    assert(proxy_dns_reclaimable(&p));
    assert(!proxy_dns_pending_expired(&p,20000,10000)); /* No pending ID. */
    p=(proxy_dns_pending_t){0};
    timed1[2]=1; timed2[2]=1;
    proxy_dns_sent_at(&p,timed1,12,100);
    assert(!proxy_dns_pending_expired(&p,10099,10000));
    assert(proxy_dns_pending_expired(&p,10100,10000));
    proxy_dns_sent_at(&p,timed1,12,10100); /* Retry retains first-send deadline. */
    assert(proxy_dns_pending_expired(&p,10100,10000));
    proxy_dns_sent_at(&p,timed2,12,10090); /* Fresh multiplexed query pins flow. */
    assert(!proxy_dns_pending_expired(&p,10100,10000));
    timed2[2]=0x81; proxy_dns_replied_at(&p,timed2,12,10101);
    assert(proxy_dns_pending_expired(&p,10101,10000));
    assert(!proxy_dns_pending_expired(&p,99,10000)); /* Clock bounds. */
    assert(!proxy_dns_pending_expired(&p,20000,0));
    p.untracked=true;
    assert(!proxy_dns_pending_expired(&p,20000,10000));
    p=(proxy_dns_pending_t){0};
    request(&p,1); request(&p,2); request(&p,1); /* Retries keep the same ID. */
    assert(p.count==2 && !proxy_dns_reclaimable(&p));
    response(&p,42); response(&p,2); response(&p,2);
    assert(p.count==1 && !proxy_dns_reclaimable(&p));
    response(&p,1);
    assert(proxy_dns_reclaimable(&p));
    request(&p,3); /* Reused client port must wait for the new response. */
    assert(!proxy_dns_reclaimable(&p));
    response(&p,3); assert(proxy_dns_reclaimable(&p));

    p=(proxy_dns_pending_t){0};
    for (unsigned i=0;i<PROXY_DNS_PENDING_IDS;i++) request(&p,i);
    for (unsigned i=0;i<PROXY_DNS_PENDING_IDS;i++) response(&p,i);
    assert(proxy_dns_reclaimable(&p));
    p=(proxy_dns_pending_t){0};
    for (unsigned i=0;i<=PROXY_DNS_PENDING_IDS;i++) request(&p,i);
    for (unsigned i=0;i<=PROXY_DNS_PENDING_IDS;i++) response(&p,i);
    assert(!proxy_dns_reclaimable(&p)); /* Overflow conservatively pins flow. */

    p=(proxy_dns_pending_t){0}; request(&p,8);
    uint8_t short_header[1]={0};
    proxy_dns_replied(&p,short_header,sizeof(short_header));
    assert(!proxy_dns_reclaimable(&p));
    uint8_t query[12]={0,8,1,0,0,1};
    proxy_dns_replied(&p,query,sizeof(query)); /* QR=0 is not an answer. */
    assert(!proxy_dns_reclaimable(&p));
    response(&p,8); assert(proxy_dns_reclaimable(&p));
    proxy_dns_sent(&p,short_header,sizeof(short_header));
    assert(!proxy_dns_reclaimable(&p)); /* Unsupported port-53 traffic stays. */
    p=(proxy_dns_pending_t){0};
    uint8_t other_opcode[12]={0,9,0x29};
    proxy_dns_sent(&p,other_opcode,sizeof(other_opcode)); response(&p,9);
    assert(!proxy_dns_reclaimable(&p));
    puts("DNS pending IDs, retry, multiplex, reuse, overflow and malformed headers: PASS");
    return 0;
}
