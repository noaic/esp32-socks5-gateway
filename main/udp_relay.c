#include "udp_relay.h"
#include "socks_udp.h"
#include "dns_flow.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#define WARN(...) ESP_LOGW("udp_proxy", __VA_ARGS__)
#define DETAIL(...) ESP_LOGD("udp_proxy", __VA_ARGS__)
static int64_t now_us(void) { return esp_timer_get_time(); }
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <time.h>
#define WARN(...) do { fprintf(stderr,__VA_ARGS__); fputc('\n',stderr); } while (0)
#define DETAIL(...) do { } while (0)
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
#endif
#ifndef UDP_HANDSHAKE_MS
#define UDP_HANDSHAKE_MS 15000
#endif
#ifndef UDP_PENDING_MS
#define UDP_PENDING_MS 3000
#endif
#ifndef UDP_IDLE_MS
#define UDP_IDLE_MS (PROXY_UDP_IDLE_SECONDS * 1000)
#endif
#ifndef UDP_DNS_IDLE_MS
#define UDP_DNS_IDLE_MS (PROXY_UDP_DNS_IDLE_SECONDS * 1000)
#endif
#ifndef UDP_DNS_PRESSURE_MS
#define UDP_DNS_PRESSURE_MS (PROXY_UDP_DNS_PRESSURE_SECONDS * 1000)
#endif
#define PENDING_DEPTH 4
#define MAX_HANDSHAKES 8
#define DNS_HANDSHAKES_RESERVED 2
#define WIRE (PROXY_UDP_MAX_PAYLOAD + 10)
typedef enum { WAIT, CONNECTING, HELLO_TX, HELLO_RX, ASSOC_TX, ASSOC_RX, READY } state_t;
typedef struct {
    uint8_t bytes[PROXY_UDP_MAX_PAYLOAD];
    uint16_t size;
    int64_t queued_us;
    bool warm; /* Admitted on READY and no association failure before send. */
} pending_t;
typedef struct {
    bool used;
    int control, udp;
    state_t state;
    proxy_udp_packet_t tuple;
    void *ap;
    uint32_t generation;
    uint32_t client_token;
    struct sockaddr_in relay;
    uint8_t request[10], response[10];
    size_t offset;
    int64_t touched_us, started_us, retry_us;
    unsigned failures, head, count;
    pending_t pending[PENDING_DEPTH];
    proxy_dns_pending_t dns;
} flow_t;
#ifdef ESP_PLATFORM
static flow_t *flows;
#else
static flow_t flows[PROXY_MAX_UDP_FLOWS];
#endif
/* Avoid multi-kilobyte automatic buffers on the embedded worker stack. */
static udp_datagram_t *working;
static uint8_t *receive_wire, *send_wire;
#ifndef ESP_PLATFORM
static udp_datagram_t host_working;
static uint8_t host_receive[WIRE+1], host_send[WIRE];
#endif
bool udp_relay_init(void)
{
#ifdef ESP_PLATFORM
    if (flows) return true;
    flow_t *f=heap_caps_calloc(PROXY_MAX_UDP_FLOWS,sizeof(*f),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    udp_datagram_t *w=heap_caps_calloc(1,sizeof(*w),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *r=heap_caps_malloc(WIRE+1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *s=heap_caps_malloc(WIRE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!f || !w || !r || !s) {
        heap_caps_free(f); heap_caps_free(w); heap_caps_free(r); heap_caps_free(s);
        return false;
    }
    flows=f; working=w; receive_wire=r; send_wire=s;
#else
    working=&host_working; receive_wire=host_receive; send_wire=host_send;
#endif
    return true;
}
static bool transient(void) { return errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR; }
static bool nonblock(int fd) { int f=fcntl(fd,F_GETFL,0); return f>=0 && fcntl(fd,F_SETFL,f|O_NONBLOCK)==0; }
static void count(const udp_relay_callbacks_t *cb,void *ctx,udp_diag_t key,unsigned amount)
{ if (amount) cb->count(ctx,key,amount); }
static void count_port(const udp_relay_callbacks_t *cb,void *ctx,udp_diag_t key,uint16_t port)
{
    count(cb,ctx,key,1);
    if (port==53) count(cb,ctx,(udp_diag_t)(key+1),1);
    if (port==443) count(cb,ctx,(udp_diag_t)(key+2),1);
}
static void close_sockets(flow_t *f)
{
    /* Close UDP first; a strict relay's control EOF ends its association. */
    if (f->udp>=0) close(f->udp);
    if (f->control>=0) close(f->control);
    f->udp=f->control=-1;
}
static void release(flow_t *f,const udp_relay_callbacks_t *cb,void *ctx)
{
    close_sockets(f);
    if (f->used && cb->client_release) cb->client_release(ctx,f->client_token,f->tuple.dst_port==53);
    f->used=false;
}
static bool client_current(const udp_relay_callbacks_t *cb,void *ctx,uint32_t token)
{ return !cb->client_current || cb->client_current(ctx,token); }
static bool same_tuple(const proxy_udp_packet_t *a,const proxy_udp_packet_t *b)
{
    return a->src_ip==b->src_ip && a->dst_ip==b->dst_ip &&
           a->src_port==b->src_port && a->dst_port==b->dst_port;
}
static flow_t *find_flow(const udp_datagram_t *m,const udp_relay_callbacks_t *cb,void *ctx)
{
    flow_t *free_slot=NULL, *completed=NULL, *expired=NULL;
    int64_t now=now_us()/1000;
    unsigned ordinary=0;
    for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) {
        flow_t *f=&flows[i];
        if (!f->used) { if (!free_slot) free_slot=f; continue; }
        if (same_tuple(&f->tuple,&m->tuple) && f->client_token==m->client_token) return f;
        if (f->tuple.dst_port!=53) ordinary++;
        if (!f->count && f->tuple.dst_port==53 && proxy_dns_reclaimable(&f->dns) &&
            (!completed || f->touched_us<completed->touched_us)) completed=f;
        if (!f->count && f->state==READY && f->tuple.dst_port==53 &&
            proxy_dns_pending_expired(&f->dns,now,UDP_DNS_PRESSURE_MS) &&
            (!expired || f->touched_us<expired->touched_us)) expired=f;
    }
    if (m->tuple.dst_port!=53 && ordinary>=PROXY_MAX_UDP_FLOWS-PROXY_UDP_DNS_RESERVED) {
        count(cb,ctx,UDP_NON_DNS_LIMIT,1); return NULL;
    }
    /* Do not let missing upstream DNS replies pin every reserved DNS slot
     * until the normal UDP idle TTL. Only a new DNS flow can reclaim a fully
     * timed-out, tracked DNS association; queued/handshaking/fresh queries and
     * ordinary UDP flows keep their existing lifetime. Close both sockets to
     * retire the old association together with its transaction state. */
    flow_t *victim=!free_slot?(completed?completed:(m->tuple.dst_port==53?expired:NULL)):NULL;
    if (!free_slot && !victim) { count(cb,ctx,UDP_FLOW_FULL,1); return NULL; }
    /* Same-owner replacement first returns its quota. A different owner's
     * completed DNS must never be evicted for a quota-rejected claimant. */
    if (victim && victim->client_token==m->client_token) {
        release(victim,cb,ctx); free_slot=victim;
        count(cb,ctx,victim==completed?UDP_DNS_RECLAIMED:UDP_DNS_PRESSURE_RECLAIMED,1);
        victim=NULL;
    }
    if (cb->client_acquire && !cb->client_acquire(ctx,m->client_token,m->tuple.dst_port==53)) {
        count(cb,ctx,UDP_FLOW_FULL,1); return NULL;
    }
    if (victim) {
        release(victim,cb,ctx); free_slot=victim;
        count(cb,ctx,victim==completed?UDP_DNS_RECLAIMED:UDP_DNS_PRESSURE_RECLAIMED,1);
    }
    *free_slot=(flow_t){.used=true,.control=-1,.udp=-1,.state=WAIT,
        .tuple=m->tuple,.ap=m->ap,.generation=m->generation,.client_token=m->client_token,.touched_us=now_us()};
    return free_slot;
}
static void expire_pending(flow_t *f,int64_t now,const udp_relay_callbacks_t *cb,void *ctx)
{
    while (f->count && now-f->pending[f->head].queued_us>=UDP_PENDING_MS*1000LL) {
        f->head=(f->head+1)%PENDING_DEPTH; f->count--;
        count(cb,ctx,UDP_PENDING_EXPIRED,1);
    }
}
static void failed(flow_t *f,const char *operation,int error,
                   const udp_relay_callbacks_t *cb,void *ctx)
{
    char remote[16]; inet_ntop(AF_INET,&f->tuple.dst_ip,remote,sizeof(remote));
    WARN("association error client_port=%u remote=%s:%u state=%d operation=%s errno=%d",
         f->tuple.src_port,remote,f->tuple.dst_port,f->state,operation,error);
    bool established=f->state==READY;
    close_sockets(f); f->state=WAIT; f->offset=0;
    f->dns=(proxy_dns_pending_t){0};
    for (unsigned i=0;i<f->count;i++) f->pending[(f->head+i)%PENDING_DEPTH].warm=false;
    /* First lost live association is retried on demand immediately; repeated
     * handshake failures back off 250/500/1000/2000/4000/5000ms, per flow. */
    if (established) f->failures=0;
    unsigned delay=f->failures?250u << (f->failures>5?5:f->failures-1):0;
    if (delay>5000) delay=5000;
    if (f->failures<7) f->failures++;
    f->retry_us=now_us()+delay*1000LL;
    if (cb->association_error) cb->association_error(ctx,!strcmp(operation,"ASSOCIATE_reply")?(error?1000+error:3000):
        !strcmp(operation,"NO_AUTH")?(error?2000+error:3000):!strcmp(operation,"timeout")?-ETIMEDOUT:error?-error:-ECONNRESET);
    count(cb,ctx,UDP_ASSOC_LOST,1);
}
static bool open_association(flow_t *f,const struct sockaddr_in *upstream)
{
    struct sockaddr_in local={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_ANY)};
    f->udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (f->udp<0 || !nonblock(f->udp) || bind(f->udp,(struct sockaddr *)&local,sizeof(local))) return false;
    f->control=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (f->control<0 || !nonblock(f->control)) return false;
    int one=1, idle=15, interval=5, probes=3;
    if (setsockopt(f->control,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one))) return false;
    /* Keepalive is link failure detection, not application data or an Xray
     * UDP idle-timer refresh. A normal server idle EOF is handled on demand. */
    setsockopt(f->control,SOL_SOCKET,SO_KEEPALIVE,&one,sizeof(one));
    setsockopt(f->control,IPPROTO_TCP,TCP_KEEPIDLE,&idle,sizeof(idle));
    setsockopt(f->control,IPPROTO_TCP,TCP_KEEPINTVL,&interval,sizeof(interval));
    setsockopt(f->control,IPPROTO_TCP,TCP_KEEPCNT,&probes,sizeof(probes));
    int rc=connect(f->control,(const struct sockaddr *)upstream,sizeof(*upstream));
    if (rc && errno!=EINPROGRESS) return false;
    f->state=rc?CONNECTING:HELLO_TX; f->started_us=now_us(); f->offset=0;
    return true;
}
static bool make_request(flow_t *f)
{
    struct sockaddr_in local, client; socklen_t size=sizeof(local);
    if (getsockname(f->udp,(struct sockaddr *)&local,&size)) return false;
    size=sizeof(client);
    if (getsockname(f->control,(struct sockaddr *)&client,&size) || !client.sin_addr.s_addr || !local.sin_port) return false;
    memcpy(f->request,(uint8_t[]){5,3,0,1},4);
    memcpy(f->request+4,&client.sin_addr.s_addr,4);
    memcpy(f->request+8,&local.sin_port,2);
    return true;
}
static void advance_control(flow_t *f,fd_set *rd,fd_set *wr,fd_set *ex,
                            const struct sockaddr_in *upstream,
                            const udp_relay_callbacks_t *cb,void *ctx,udp_relay_stats_t *stats)
{
    if (FD_ISSET(f->control,ex)) {
        int error=0; socklen_t size=sizeof(error);
        if (getsockopt(f->control,SOL_SOCKET,SO_ERROR,&error,&size)) error=errno;
        failed(f,"control_exception",error,cb,ctx); return;
    }
    if (f->state==CONNECTING && FD_ISSET(f->control,wr)) {
        int error=0; socklen_t size=sizeof(error);
        int rc=getsockopt(f->control,SOL_SOCKET,SO_ERROR,&error,&size);
        if (rc || error) { failed(f,"connect",rc?errno:error,cb,ctx); return; }
        f->state=HELLO_TX; f->offset=0;
    }
    if ((f->state==HELLO_TX || f->state==ASSOC_TX) && FD_ISSET(f->control,wr)) {
        static const uint8_t hello[]={5,1,0};
        size_t length=f->state==HELLO_TX?3:10;
        const uint8_t *data=f->state==HELLO_TX?hello:f->request;
        int n=send(f->control,data+f->offset,length-f->offset,0);
        if (n>0) {
            f->offset+=n;
            if (f->offset==length) { f->state=f->state==HELLO_TX?HELLO_RX:ASSOC_RX; f->offset=0; }
        } else if (!n || !transient()) { failed(f,"handshake_send",n<0?errno:0,cb,ctx); return; }
    }
    if ((f->state==HELLO_RX || f->state==ASSOC_RX) && FD_ISSET(f->control,rd)) {
        size_t length=f->state==HELLO_RX?2:10;
        int n=recv(f->control,f->response+f->offset,length-f->offset,0);
        if (n>0) {
            f->offset+=n;
            if (f->state==ASSOC_RX && f->offset>=4 &&
                (f->response[0]!=5 || f->response[1] || f->response[2] || f->response[3]!=1)) {
                failed(f,"ASSOCIATE_reply",f->response[1],cb,ctx); return;
            }
            if (f->offset==length) {
                if (f->state==HELLO_RX) {
                    if (f->response[0]!=5 || f->response[1]) { failed(f,"NO_AUTH",f->response[1],cb,ctx); return; }
                    if (!make_request(f)) { failed(f,"local_address",errno,cb,ctx); return; }
                    f->state=ASSOC_TX; f->offset=0;
                } else {
                    f->relay=(struct sockaddr_in){.sin_family=AF_INET};
                    memcpy(&f->relay.sin_addr.s_addr,f->response+4,4);
                    memcpy(&f->relay.sin_port,f->response+8,2);
                    if (!f->relay.sin_addr.s_addr) f->relay.sin_addr=upstream->sin_addr;
                    uint32_t ip=ntohl(f->relay.sin_addr.s_addr);
                    /* Allow loopback only in host tests; embedded relay must be routable. */
#ifdef ESP_PLATFORM
                    bool loopback=(ip>>24)==127;
#else
                    bool loopback=false;
#endif
                    if (!f->relay.sin_port || !(ip>>24) || (ip>>24)>=224 || loopback) {
                        failed(f,"relay_address",0,cb,ctx); return;
                    }
                    if (connect(f->udp,(struct sockaddr *)&f->relay,sizeof(f->relay))) {
                        failed(f,"udp_connect",errno,cb,ctx); return;
                    }
                    f->state=READY; f->failures=0;
                    uint64_t elapsed=now_us()-f->started_us;
                    stats->associations++; stats->association_total_us+=elapsed;
                    if (elapsed>stats->association_max_us) stats->association_max_us=elapsed;
                    DETAIL("association ready client_port=%u relay_port=%u",f->tuple.src_port,ntohs(f->relay.sin_port));
                    FD_CLR(f->control,rd); /* No control application bytes in this readiness snapshot. */
                }
            }
        } else if (!n || !transient()) { failed(f,"handshake_receive",n<0?errno:0,cb,ctx); return; }
    }
    if (f->state==READY && FD_ISSET(f->control,rd)) {
        uint8_t byte; int n=recv(f->control,&byte,1,0);
        if (n>=0 || !transient()) {
            failed(f,n==0?"control_eof":n>0?"unexpected_control_data":"control_receive",n<0?errno:0,cb,ctx);
        }
    }
}
static void receive(flow_t *f,const udp_relay_callbacks_t *cb,void *ctx,udp_relay_stats_t *stats)
{
    /* Four packets per ready socket, bounded and round-robin across flows. */
    for (unsigned i=0;i<4;i++) {
        int n=recv(f->udp,receive_wire,WIRE+1,0);
        if (n<0) {
            if (!transient()) { count(cb,ctx,UDP_RECEIVE_DROP,1); failed(f,"udp_receive",errno,cb,ctx); }
            return;
        }
        uint32_t remote; uint16_t port; const uint8_t *payload; size_t length;
        if (n>WIRE || !socks_udp_decode(receive_wire,(size_t)n,&remote,&port,&payload,&length) ||
            remote!=f->tuple.dst_ip || port!=f->tuple.dst_port || length>PROXY_UDP_MAX_PAYLOAD) {
            count(cb,ctx,UDP_BAD_REPLY,1); continue;
        }
        count_port(cb,ctx,UDP_REPLY,port);
        f->touched_us=now_us();
        working->tuple=f->tuple; working->ap=f->ap; working->generation=f->generation;
        working->client_token=f->client_token;
        if (cb->reply(ctx,working,payload,length) && port==53) {
            int64_t rtt=proxy_dns_replied_at(&f->dns,payload,length,f->touched_us/1000);
            if (rtt>=0) {
                stats->dns_replies++; stats->dns_rtt_total_ms+=rtt;
                if ((uint64_t)rtt>stats->dns_rtt_max_ms) stats->dns_rtt_max_ms=rtt;
            }
        }
    }
}
static void send_pending(flow_t *f,const udp_relay_callbacks_t *cb,void *ctx,udp_relay_stats_t *stats)
{
    for (unsigned i=0;i<PENDING_DEPTH && f->state==READY && f->count;i++) {
        pending_t *p=&f->pending[f->head];
        size_t size=socks_udp_encode(send_wire,WIRE,f->tuple.dst_ip,f->tuple.dst_port,p->bytes,p->size);
        int n=send(f->udp,send_wire,size,0);
        if (n<0 && transient()) return; /* Retain EAGAIN, unlike the old drop path. */
        if (n!=(int)size) {
            count(cb,ctx,UDP_SEND_DROP,1); failed(f,"udp_send",n<0?errno:0,cb,ctx); return;
        }
        int64_t now=now_us(); uint64_t wait=now-p->queued_us;
        stats->dispatches++; stats->queue_wait_total_us+=wait;
        if (wait>stats->queue_wait_max_us) stats->queue_wait_max_us=wait;
        if (f->tuple.dst_port==53) {
            stats->dns_dispatches++; stats->dns_wait_total_us+=wait;
            if (wait>stats->dns_wait_max_us) stats->dns_wait_max_us=wait;
        }
        if (p->warm) {
            stats->warm_dispatches++; stats->warm_wait_total_us+=wait;
            if (wait>stats->warm_wait_max_us) stats->warm_wait_max_us=wait;
        }
        f->touched_us=now;
        if (f->tuple.dst_port==53) proxy_dns_sent_at(&f->dns,p->bytes,p->size,now/1000);
        count_port(cb,ctx,UDP_TX,f->tuple.dst_port);
        if (cb->client_traffic) cb->client_traffic(ctx,f->client_token,true,p->size);
        f->head=(f->head+1)%PENDING_DEPTH; f->count--;
    }
}
void udp_relay_run(const char *upstream,uint16_t port,int wake_fd,
                   const udp_relay_callbacks_t *cb,void *ctx)
{
    if (!udp_relay_init()) return;
    struct sockaddr_in server={.sin_family=AF_INET,.sin_port=htons(port)};
    if (inet_pton(AF_INET,upstream,&server.sin_addr)!=1) return;
    for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) flows[i]=(flow_t){.control=-1,.udp=-1};
    uint32_t epoch=0; unsigned cursor=0;
    udp_relay_stats_t stats={0}; int64_t last_stats=now_us();
    while (!cb->stop || !cb->stop(ctx)) {
        bool online; uint32_t gen; cb->snapshot(ctx,&online,&gen);
        if (!online || gen!=epoch) {
            for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) if (flows[i].used) release(&flows[i],cb,ctx);
            epoch=gen;
        }
        for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) if (flows[i].used && !client_current(cb,ctx,flows[i].client_token)) release(&flows[i],cb,ctx);
        /* Four preferred DNS admissions, then four ordinary admissions. Empty
         * queues lend their budget to the other class; neither can starve. */
        for (unsigned i=0;i<8;i++) {
            bool dns=i<4;
            if (!cb->take(ctx,dns,working) && !cb->take(ctx,!dns,working)) break;
            if (!online) { count(cb,ctx,UDP_NOT_READY,1); continue; }
            if (working->generation!=epoch || !client_current(cb,ctx,working->client_token)) { count(cb,ctx,UDP_STALE,1); continue; }
            if (working->tuple.payload_length>PROXY_UDP_MAX_PAYLOAD || !working->tuple.dst_port) {
                count(cb,ctx,UDP_BAD_INPUT,1); continue;
            }
            if (now_us()-working->queued_us>=UDP_PENDING_MS*1000LL) {
                count(cb,ctx,UDP_PENDING_EXPIRED,1); continue;
            }
            flow_t *f=find_flow(working,cb,ctx); if (!f) continue;
            expire_pending(f,now_us(),cb,ctx);
            if (f->count==PENDING_DEPTH) { count(cb,ctx,UDP_PENDING_FULL,1); continue; }
            pending_t *p=&f->pending[(f->head+f->count)%PENDING_DEPTH];
            p->size=working->tuple.payload_length; p->queued_us=working->queued_us;
            p->warm=f->state==READY;
            memcpy(p->bytes,working->bytes,p->size); f->count++; f->touched_us=now_us();
        }
        int64_t now=now_us(); unsigned handshakes=0, ordinary_handshakes=0;
        for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) {
            flow_t *f=&flows[i]; if (!f->used) continue;
            expire_pending(f,now,cb,ctx);
            int64_t idle=(f->tuple.dst_port==53 && proxy_dns_reclaimable(&f->dns)?UDP_DNS_IDLE_MS:UDP_IDLE_MS)*1000LL;
            if (!f->count && (now-f->touched_us>=idle || (f->state==WAIT && now>=f->retry_us))) { release(f,cb,ctx); continue; }
            if (f->control>=0 && f->state!=READY && now-f->started_us>=UDP_HANDSHAKE_MS*1000LL) {
                failed(f,"handshake_timeout",0,cb,ctx);
            }
            if (f->state!=WAIT && f->state!=READY) {
                handshakes++; if (f->tuple.dst_port!=53) ordinary_handshakes++;
            }
        }
        /* Start DNS first; leave two of eight handshake slots for DNS during
         * an unreachable ordinary UDP burst. A WAIT flow has no sockets. */
        for (unsigned pass=0;pass<2;pass++) for (unsigned n=0;n<PROXY_MAX_UDP_FLOWS;n++) {
            flow_t *f=&flows[(cursor+n)%PROXY_MAX_UDP_FLOWS];
            bool dns=f->tuple.dst_port==53;
            if (!f->used || f->state!=WAIT || !f->count || now<f->retry_us || dns!=(pass==0)) continue;
            if (handshakes>=MAX_HANDSHAKES || (!dns && ordinary_handshakes>=MAX_HANDSHAKES-DNS_HANDSHAKES_RESERVED)) continue;
            if (!open_association(f,&server)) {
                int error=errno; count(cb,ctx,UDP_FLOW_SOCKET,1); failed(f,"socket_or_connect",error,cb,ctx);
            } else { handshakes++; if (!dns) ordinary_handshakes++; }
        }
        fd_set rd,wr,ex; FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(wake_fd,&rd); int maximum=wake_fd;
        for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) {
            flow_t *f=&flows[i]; if (!f->used || f->control<0) continue;
            FD_SET(f->control,&ex); if (f->control>maximum) maximum=f->control;
            if (f->state==CONNECTING || f->state==HELLO_TX || f->state==ASSOC_TX) FD_SET(f->control,&wr);
            if (f->state==HELLO_RX || f->state==ASSOC_RX || f->state==READY) FD_SET(f->control,&rd);
            if (f->state==READY) {
                FD_SET(f->udp,&rd); if (f->count) FD_SET(f->udp,&wr);
                if (f->udp>maximum) maximum=f->udp;
            }
        }
        int64_t maintenance_wait=cb->maintain?cb->maintain(ctx):100000;
        if (maintenance_wait<0) maintenance_wait=0;
        if (maintenance_wait>100000) maintenance_wait=100000;
        struct timeval timeout={.tv_sec=0,.tv_usec=cb->pending(ctx)?0:maintenance_wait};
        int selected=select(maximum+1,&rd,&wr,&ex,&timeout);
        if (selected<0 && errno==EINTR) continue;
        if (selected>=0 && FD_ISSET(wake_fd,&rd)) { uint64_t signal; (void)read(wake_fd,&signal,sizeof(signal)); }
        /* Recheck immediately after select: old descriptors/messages must not
         * cross a client or STA generation change during the blocking wait. */
        cb->snapshot(ctx,&online,&gen);
        if (!online || gen!=epoch) continue;
        int select_error=selected<0?errno:0;
        /* All selected sockets are consumed before the next iteration can
         * create/reuse a descriptor. DNS replies precede ordinary replies. */
        for (unsigned pass=0;pass<2;pass++) for (unsigned n=0;n<PROXY_MAX_UDP_FLOWS;n++) {
            flow_t *f=&flows[(cursor+n)%PROXY_MAX_UDP_FLOWS];
            if (!f->used || f->control<0 || (f->tuple.dst_port==53)!=(pass==0)) continue;
            if (!client_current(cb,ctx,f->client_token)) { release(f,cb,ctx); continue; }
            if (selected<0) { failed(f,"select",select_error,cb,ctx); continue; }
            advance_control(f,&rd,&wr,&ex,&server,cb,ctx,&stats);
            if (f->state==READY && FD_ISSET(f->udp,&rd)) receive(f,cb,ctx,&stats);
            if (f->state==READY) send_pending(f,cb,ctx,&stats);
        }
        cursor=(cursor+1)%PROXY_MAX_UDP_FLOWS;
        if (now_us()-last_stats>=10000000) {
            stats.active=stats.ready=stats.handshake=0;
            for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) if (flows[i].used) {
                stats.active++; if (flows[i].state==READY) stats.ready++;
                else if (flows[i].state!=WAIT) stats.handshake++;
            }
            cb->stats(ctx,&stats); stats=(udp_relay_stats_t){0}; last_stats=now_us();
        }
    }
    for (unsigned i=0;i<PROXY_MAX_UDP_FLOWS;i++) if (flows[i].used) release(&flows[i],cb,ctx);
}
