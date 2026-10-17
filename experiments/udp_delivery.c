#include "udp_delivery.h"
#include <string.h>

static void count(udp_delivery_pool_t *p,udp_diag_t key)
{ p->ops.count(p->ctx,key,1); }
static void release(udp_delivery_slot_t *s)
{
    udp_delivery_pool_t *p=s->pool;
    p->ops.lock(p->ctx); s->state=UDP_SLOT_FREE; p->ops.unlock(p->ctx);
}
static void expired(udp_delivery_slot_t *s)
{
    count(s->pool,UDP_DELIVERY_EXPIRED); count(s->pool,UDP_OUTPUT_DROP); release(s);
}
static bool valid(udp_delivery_slot_t *s,int64_t now)
{
    if (!s->pool->ops.current(s->pool->ctx,s->message.generation)) {
        count(s->pool,UDP_STALE); release(s); return false;
    }
    if (now-s->accepted_us>=UDP_DELIVERY_TTL_US) { expired(s); return false; }
    return true;
}
static void delivered(void *arg)
{
    udp_delivery_slot_t *s=arg; udp_delivery_pool_t *p=s->pool;
    p->ops.lock(p->ctx);
    bool posted=s->state==UDP_SLOT_POSTED;
    if (posted) s->state=UDP_SLOT_RUNNING;
    p->ops.unlock(p->ctx);
    if (!posted || !valid(s,p->ops.now_us(p->ctx))) return;
    ++s->attempts;
    udp_emit_result_t result=p->ops.emit(p->ctx,&s->message);
    if (result==UDP_EMIT_STALE) { count(p,UDP_STALE); release(s); return; }
    if (result==UDP_EMIT_OK) {
        count(p,UDP_DELIVERED);
        if (s->attempts>1) count(p,UDP_OUTPUT_RECOVERED);
        release(s); return;
    }
    static const udp_diag_t reasons[]={UDP_OUTPUT_OTHER,UDP_OUTPUT_ALLOC,UDP_OUTPUT_MEM,
        UDP_OUTPUT_BUF,UDP_OUTPUT_IF,UDP_OUTPUT_DOWN,UDP_OUTPUT_OTHER};
    count(p,(unsigned)result<sizeof(reasons)/sizeof(reasons[0])?reasons[result]:UDP_OUTPUT_OTHER);
    bool retry=result==UDP_EMIT_ALLOC || result==UDP_EMIT_MEM || result==UDP_EMIT_BUF;
    int64_t now=p->ops.now_us(p->ctx);
    if (retry && s->attempts<UDP_DELIVERY_MAX_ATTEMPTS && now-s->accepted_us<UDP_DELIVERY_TTL_US) {
        p->ops.lock(p->ctx);
        s->due_us=now+UDP_DELIVERY_RETRY_US; s->state=UDP_SLOT_WAIT;
        p->ops.unlock(p->ctx);
        count(p,UDP_OUTPUT_RETRY); p->ops.wake(p->ctx);
    } else {
        if (retry) count(p,UDP_DELIVERY_EXHAUSTED);
        count(p,UDP_OUTPUT_DROP); release(s);
    }
}
/* Caller has claimed WAIT as POSTED. No lock is held during post/emit. */
static void post(udp_delivery_slot_t *s)
{
    udp_delivery_pool_t *p=s->pool;
    if (!valid(s,p->ops.now_us(p->ctx))) return;
    if (!p->ops.post(p->ctx,delivered,s)) {
        count(p,UDP_CALLBACK_FULL);
        p->ops.lock(p->ctx);
        s->due_us=p->ops.now_us(p->ctx)+UDP_DELIVERY_RETRY_US;
        s->state=UDP_SLOT_WAIT;
        p->ops.unlock(p->ctx);
        p->ops.wake(p->ctx);
    }
    /* A successful post may already have run/released the slot. */
}
void udp_delivery_init(udp_delivery_pool_t *p,const udp_delivery_ops_t *ops,void *ctx)
{
    memset(p,0,sizeof(*p)); p->ops=*ops; p->ctx=ctx;
    for (unsigned i=0;i<UDP_DELIVERY_DEPTH;i++) p->slots[i].pool=p;
}
bool udp_delivery_submit(udp_delivery_pool_t *p,const udp_datagram_t *m,
                         const uint8_t *data,size_t length)
{
    if (length>PROXY_UDP_MAX_PAYLOAD) { count(p,UDP_BAD_REPLY); return false; }
    if (!p->ops.current(p->ctx,m->generation)) { count(p,UDP_STALE); return false; }
    udp_delivery_slot_t *s=NULL;
    unsigned limit=m->tuple.dst_port==53?UDP_DELIVERY_DEPTH:UDP_DELIVERY_DEPTH-UDP_DELIVERY_DNS_RESERVED;
    p->ops.lock(p->ctx);
    for (unsigned i=0;i<limit;i++) if (p->slots[i].state==UDP_SLOT_FREE) {
        s=&p->slots[i]; s->state=UDP_SLOT_FILLING; break;
    }
    p->ops.unlock(p->ctx);
    if (!s) { count(p,UDP_DELIVERY_FULL); return false; }
    s->message.tuple=m->tuple; s->message.tuple.payload_length=length;
    s->message.ap=m->ap; s->message.generation=m->generation;
    s->message.client_token=m->client_token;
    memcpy(s->message.bytes,data,length);
    s->attempts=0; s->accepted_us=p->ops.now_us(p->ctx);
    p->ops.lock(p->ctx); s->state=UDP_SLOT_POSTED; p->ops.unlock(p->ctx);
    post(s);
    return true; /* Owns bytes even if posting awaits a retry. */
}
int64_t udp_delivery_pump(udp_delivery_pool_t *p)
{
    unsigned budget=0;
    /* Two DNS and two ordinary opportunities; a missing class lends its
     * budget. Never perform an unbounded retry loop in either context. */
    for (unsigned pass=0;pass<2 && budget<4;pass++) {
        unsigned served=0;
        for (unsigned n=0;n<UDP_DELIVERY_DEPTH && budget<4;n++) {
            udp_delivery_slot_t *s=&p->slots[(p->cursor+n)%UDP_DELIVERY_DEPTH];
            int64_t now=p->ops.now_us(p->ctx);
            p->ops.lock(p->ctx);
            bool due=s->state==UDP_SLOT_WAIT &&
                (now>=s->due_us || now-s->accepted_us>=UDP_DELIVERY_TTL_US);
            bool eligible=due && (s->message.tuple.dst_port==53)==(pass==0);
            if (eligible) s->state=UDP_SLOT_POSTED;
            p->ops.unlock(p->ctx);
            if (!eligible) continue;
            post(s); budget++; served++;
            if (pass==0 && served==2) break;
        }
    }
    /* Lend spare budget to any due slots. */
    for (unsigned n=0;n<UDP_DELIVERY_DEPTH && budget<4;n++) {
        udp_delivery_slot_t *s=&p->slots[(p->cursor+n)%UDP_DELIVERY_DEPTH];
        int64_t now=p->ops.now_us(p->ctx);
        p->ops.lock(p->ctx);
        bool due=s->state==UDP_SLOT_WAIT &&
            (now>=s->due_us || now-s->accepted_us>=UDP_DELIVERY_TTL_US);
        if (due) s->state=UDP_SLOT_POSTED;
        p->ops.unlock(p->ctx);
        if (due) { post(s); budget++; }
    }
    p->cursor=(p->cursor+1)%UDP_DELIVERY_DEPTH;
    int64_t now=p->ops.now_us(p->ctx), wait=100000;
    p->ops.lock(p->ctx);
    for (unsigned i=0;i<UDP_DELIVERY_DEPTH;i++) {
        udp_delivery_slot_t *s=&p->slots[i];
        if (s->state!=UDP_SLOT_WAIT) continue;
        int64_t due=s->due_us<s->accepted_us+UDP_DELIVERY_TTL_US?s->due_us:s->accepted_us+UDP_DELIVERY_TTL_US;
        int64_t delay=due>now?due-now:0;
        if (delay<wait) wait=delay;
    }
    p->ops.unlock(p->ctx); return wait;
}
