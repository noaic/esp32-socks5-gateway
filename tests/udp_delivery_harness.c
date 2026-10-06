/* Run the production delivery pool with delayed/inline TCPIP callbacks and
 * controlled local TX failures. No ESP32, sockets or external network needed. */
#include "udp_delivery.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct { void (*fn)(void *); void *arg; } callback_t;
typedef struct {
    udp_delivery_pool_t pool;
    callback_t callbacks[64];
    unsigned head,queued,posts,emits,wakes,counts[UDP_DIAG_COUNT];
    unsigned reject_posts,fail_emits;
    udp_emit_result_t failure;
    bool locked,inline_post,online;
    uint32_t generation;
    int64_t now;
    udp_datagram_t emitted;
} fixture_t;
static fixture_t f;
static void lock(void *ctx) { fixture_t *x=ctx; assert(!x->locked); x->locked=true; }
static void unlock(void *ctx) { fixture_t *x=ctx; assert(x->locked); x->locked=false; }
static int64_t clock_us(void *ctx) { return ((fixture_t *)ctx)->now; }
static bool current(void *ctx,uint32_t gen)
{
    fixture_t *x=ctx;
    /* Real adapter takes the same lock: caller must not already hold it. */
    lock(x); bool result=x->online && x->generation==gen; unlock(x); return result;
}
static bool post(void *ctx,void (*fn)(void *),void *arg)
{
    fixture_t *x=ctx; assert(!x->locked); x->posts++;
    if (x->reject_posts) { x->reject_posts--; return false; }
    if (x->inline_post) fn(arg);
    else {
        assert(x->queued<64);
        x->callbacks[(x->head+x->queued)%64]=(callback_t){fn,arg}; x->queued++;
    }
    return true;
}
static udp_emit_result_t emit(void *ctx,const udp_datagram_t *m)
{
    fixture_t *x=ctx; assert(!x->locked); x->emits++; x->emitted=*m;
    if (x->fail_emits) { x->fail_emits--; return x->failure; }
    return UDP_EMIT_OK;
}
static void count(void *ctx,udp_diag_t key,unsigned amount)
{ fixture_t *x=ctx; assert(key<UDP_DIAG_COUNT); x->counts[key]+=amount; }
static void wake(void *ctx) { ((fixture_t *)ctx)->wakes++; }
static void setup(void)
{
    memset(&f,0,sizeof(f)); f.online=true; f.generation=1;
    const udp_delivery_ops_t ops={.lock=lock,.unlock=unlock,.now_us=clock_us,
        .current=current,.post=post,.emit=emit,.count=count,.wake=wake};
    udp_delivery_init(&f.pool,&ops,&f);
}
static bool submit(uint16_t port)
{
    uint8_t data[]={0x12,0x34,0x80,0x80,0,1,0,0,0,0,0,0,0xff};
    udp_datagram_t m={.tuple={.src_ip=0x0204a8c0,.dst_ip=0x01010101,
        .src_port=54321,.dst_port=port},.generation=f.generation,.ap=(void *)&f};
    bool result=udp_delivery_submit(&f.pool,&m,data,sizeof(data));
    /* Caller reuses both inputs immediately. Delivery must own its copy. */
    memset(data,0,sizeof(data)); memset(&m,0,sizeof(m)); return result;
}
static void run_one(void)
{
    assert(f.queued);
    callback_t cb=f.callbacks[f.head]; f.head=(f.head+1)%64; f.queued--;
    cb.fn(cb.arg);
}
static void run_all(void) { while (f.queued) run_one(); }
static unsigned occupied(void)
{
    unsigned n=0;
    for (unsigned i=0;i<UDP_DELIVERY_DEPTH;i++) n+=f.pool.slots[i].state!=UDP_SLOT_FREE;
    return n;
}
static void payload(void)
{
    const uint8_t expected[]={0x12,0x34,0x80,0x80,0,1,0,0,0,0,0,0,0xff};
    assert(f.emitted.tuple.payload_length==sizeof(expected));
    assert(!memcmp(f.emitted.bytes,expected,sizeof(expected)));
    assert(f.emitted.tuple.src_ip==0x0204a8c0 && f.emitted.tuple.dst_ip==0x01010101);
    assert(f.emitted.tuple.src_port==54321 && f.emitted.ap==(void *)&f);
}
static void successful_once(void)
{
    setup(); assert(submit(53)); assert(occupied()==1); run_one(); payload();
    assert(f.emits==1 && f.counts[UDP_DELIVERED]==1 && occupied()==0);
    for (unsigned i=0;i<20;i++) { f.now+=20000; udp_delivery_pump(&f.pool); }
    assert(f.emits==1 && f.posts==1 && f.queued==0);
}
static void temporary_recovery(udp_emit_result_t result,udp_diag_t reason)
{
    setup(); f.failure=result; f.fail_emits=2;
    assert(submit(53)); run_one();
    assert(udp_delivery_pump(&f.pool)==20000 && f.queued==0);
    f.now=19999; assert(udp_delivery_pump(&f.pool)==1 && f.queued==0);
    f.now=20000; udp_delivery_pump(&f.pool); run_one();
    f.now=40000; udp_delivery_pump(&f.pool); run_one(); payload();
    assert(f.emits==3 && f.counts[reason]==2 && f.counts[UDP_OUTPUT_RETRY]==2);
    assert(f.counts[UDP_DELIVERED]==1 && f.counts[UDP_OUTPUT_RECOVERED]==1);
    assert(f.counts[UDP_OUTPUT_DROP]==0 && occupied()==0);
    f.now=1000000; udp_delivery_pump(&f.pool); assert(f.emits==3 && f.queued==0);
}
static void permanent_failure(udp_emit_result_t result,udp_diag_t reason)
{
    setup(); f.failure=result; f.fail_emits=1; assert(submit(443)); run_one();
    assert(f.emits==1 && f.counts[reason]==1 && f.counts[UDP_OUTPUT_DROP]==1);
    assert(f.counts[UDP_OUTPUT_RETRY]==0 && occupied()==0);
    f.now=1000000; udp_delivery_pump(&f.pool); assert(f.posts==1);
}
static void exhausted(void)
{
    setup(); f.failure=UDP_EMIT_BUF; f.fail_emits=100;
    assert(submit(53)); run_one();
    for (unsigned i=1;i<UDP_DELIVERY_MAX_ATTEMPTS;i++) {
        f.now+=UDP_DELIVERY_RETRY_US; udp_delivery_pump(&f.pool); run_one();
    }
    assert(f.emits==4 && f.counts[UDP_OUTPUT_RETRY]==3);
    assert(f.counts[UDP_DELIVERY_EXHAUSTED]==1 && f.counts[UDP_OUTPUT_DROP]==1);
    assert(occupied()==0); f.now+=1000000; udp_delivery_pump(&f.pool); assert(f.posts==4);
}
static void posting_recovery(void)
{
    setup(); f.reject_posts=2; assert(submit(53)); assert(f.emits==0 && occupied()==1);
    f.now=20000; udp_delivery_pump(&f.pool); assert(f.queued==0);
    f.now=40000; udp_delivery_pump(&f.pool); run_one(); payload();
    assert(f.counts[UDP_CALLBACK_FULL]==2 && f.counts[UDP_DELIVERED]==1);
    assert(f.counts[UDP_OUTPUT_DROP]==0 && occupied()==0);
}
static void expiry(void)
{
    setup(); f.reject_posts=100; assert(submit(53));
    f.now=UDP_DELIVERY_TTL_US; udp_delivery_pump(&f.pool);
    assert(f.posts==1 && f.emits==0 && occupied()==0);
    assert(f.counts[UDP_DELIVERY_EXPIRED]==1 && f.counts[UDP_OUTPUT_DROP]==1);
    /* A posted callback cannot be reclaimed by the worker even after TTL. */
    setup(); assert(submit(53)); f.now=UDP_DELIVERY_TTL_US; udp_delivery_pump(&f.pool);
    assert(occupied()==1); run_one();
    assert(f.emits==0 && f.counts[UDP_DELIVERY_EXPIRED]==1 && occupied()==0);
    setup(); f.failure=UDP_EMIT_MEM; f.fail_emits=1; assert(submit(53)); run_one();
    f.now=UDP_DELIVERY_TTL_US-1; udp_delivery_pump(&f.pool);
    f.now=UDP_DELIVERY_TTL_US; run_one();
    assert(f.emits==1 && f.counts[UDP_DELIVERY_EXPIRED]==1 && occupied()==0);
}
static void generation_lifetime(void)
{
    setup(); for (unsigned i=0;i<UDP_DELIVERY_DEPTH;i++) assert(submit(53));
    f.generation++; udp_delivery_pump(&f.pool);
    assert(occupied()==16 && !submit(53));
    /* Old callback must release its own slot before new client can reuse it. */
    run_one(); assert(f.emits==0 && occupied()==15); assert(submit(53));
    run_all(); payload();
    assert(f.emits==1 && f.emitted.generation==2 && f.counts[UDP_STALE]==16);
    assert(occupied()==0);
    setup(); f.reject_posts=1; assert(submit(53)); f.online=false; f.now=20000;
    udp_delivery_pump(&f.pool); assert(f.emits==0 && occupied()==0 && f.counts[UDP_STALE]==1);
    assert(!submit(53) && occupied()==0);
    /* Adapter may detect a change between initial validation and send. */
    setup(); f.failure=UDP_EMIT_STALE; f.fail_emits=1; assert(submit(53)); run_one();
    assert(f.counts[UDP_STALE]==1 && f.counts[UDP_OUTPUT_DROP]==0 && occupied()==0);
}
static void reserve_and_budget(void)
{
    setup();
    for (unsigned i=0;i<12;i++) assert(submit(443));
    assert(!submit(443));
    for (unsigned i=0;i<4;i++) assert(submit(53));
    assert(!submit(53)); run_all(); assert(f.counts[UDP_DELIVERED]==16 && occupied()==0);
    setup(); f.reject_posts=16;
    for (unsigned i=0;i<12;i++) assert(submit(443));
    for (unsigned i=0;i<4;i++) assert(submit(53));
    f.now=20000;
    assert(udp_delivery_pump(&f.pool)==0 && f.queued==4);
    run_one(); assert(f.emitted.tuple.dst_port==53);
    run_one(); assert(f.emitted.tuple.dst_port==53);
    run_one(); assert(f.emitted.tuple.dst_port==443);
    run_one(); assert(f.emitted.tuple.dst_port==443);
    for (unsigned i=0;i<3;i++) { udp_delivery_pump(&f.pool); assert(f.queued==4); run_all(); }
    assert(f.counts[UDP_DELIVERED]==16 && occupied()==0);
}
static void inline_callback(void)
{
    setup(); f.inline_post=true;
    for (unsigned i=0;i<1000;i++) assert(submit(53));
    assert(f.counts[UDP_DELIVERED]==1000 && occupied()==0);
    f.failure=UDP_EMIT_MEM; f.fail_emits=1; assert(submit(53));
    f.now=20000; udp_delivery_pump(&f.pool);
    assert(f.counts[UDP_DELIVERED]==1001 && f.counts[UDP_OUTPUT_RECOVERED]==1 && occupied()==0);
}
static void invalid_size(void)
{
    setup(); udp_datagram_t m={.generation=1}; uint8_t byte=0;
    assert(!udp_delivery_submit(&f.pool,&m,&byte,PROXY_UDP_MAX_PAYLOAD+1));
    assert(f.posts==0 && occupied()==0 && f.counts[UDP_BAD_REPLY]==1);
    assert(udp_delivery_submit(&f.pool,&m,&byte,0)); run_one();
    assert(f.emitted.tuple.payload_length==0 && f.counts[UDP_DELIVERED]==1);
}
int main(void)
{
    successful_once();
    temporary_recovery(UDP_EMIT_ALLOC,UDP_OUTPUT_ALLOC);
    temporary_recovery(UDP_EMIT_MEM,UDP_OUTPUT_MEM);
    temporary_recovery(UDP_EMIT_BUF,UDP_OUTPUT_BUF);
    permanent_failure(UDP_EMIT_IF,UDP_OUTPUT_IF);
    permanent_failure(UDP_EMIT_DOWN,UDP_OUTPUT_DOWN);
    permanent_failure(UDP_EMIT_OTHER,UDP_OUTPUT_OTHER);
    exhausted(); posting_recovery(); expiry(); generation_lifetime();
    reserve_and_budget(); inline_callback(); invalid_size();
    puts("UDP delivery: 14 cases passed"); return 0;
}
