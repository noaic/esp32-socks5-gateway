#include "udp_relay.h"
#include "udp_delivery.h"
#include "ap_clients.h"
#include <arpa/inet.h>
#include <sys/eventfd.h>
#include <sys/poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int app_socket,wake;
static atomic_bool online=true,stopping=false;
static atomic_uint generation=0;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static udp_datagram_t queue[2][32];
static unsigned head[2],size[2];
static udp_delivery_pool_t deliveries;
static bool delivery_retry;
static bool tracked;
static const uint8_t mac_a[6]={2,0,0,0,0,1},mac_b[6]={2,0,0,0,0,2};
static void connect_client(const uint8_t mac[6],const char *ip)
{ uint32_t addr; inet_pton(AF_INET,ip,&addr); proxy_client_connect(mac); proxy_client_assign(mac,addr); }
static bool owner_current(void *ctx,uint32_t token) { (void)ctx; return proxy_client_current(token); }
static bool owner_acquire(void *ctx,uint32_t token,bool dns) { (void)ctx; return proxy_client_acquire(token,true,dns); }
static void owner_release(void *ctx,uint32_t token,bool dns) { (void)ctx; proxy_client_release(token,true,dns); }
static unsigned rejected_output;
static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
static void notify(void) { uint64_t one=1; (void)write(wake,&one,sizeof(one)); }
static bool take(void *ctx,bool dns,udp_datagram_t *m)
{
    (void)ctx; unsigned q=dns?1:0; bool result=false;
    pthread_mutex_lock(&mutex);
    if (size[q]) { *m=queue[q][head[q]]; head[q]=(head[q]+1)%32; size[q]--; result=true; }
    pthread_mutex_unlock(&mutex); return result;
}
static bool pending(void *ctx)
{
    (void)ctx; pthread_mutex_lock(&mutex); bool result=size[0] || size[1];
    pthread_mutex_unlock(&mutex); return result;
}
static void snapshot(void *ctx,bool *ready,uint32_t *gen)
{ (void)ctx; *ready=atomic_load(&online); *gen=atomic_load(&generation); }
static void count(void *ctx,udp_diag_t key,unsigned amount)
{ (void)ctx; fprintf(stderr,"DIAG %d %u\n",key,amount); }
static void stats(void *ctx,const udp_relay_stats_t *s) { (void)ctx; (void)s; }
static bool stop(void *ctx) { (void)ctx; return atomic_load(&stopping); }
static bool send_reply(void *ctx,const udp_datagram_t *m,const uint8_t *data,size_t length)
{
    (void)ctx;
    if (!atomic_load(&online) || m->generation!=atomic_load(&generation)) return false;
    if (tracked && !proxy_client_current(m->client_token)) return false;
    uint8_t wire[PROXY_UDP_MAX_PAYLOAD+6];
    memcpy(wire,&m->tuple.dst_ip,4); wire[4]=m->tuple.dst_port>>8; wire[5]=m->tuple.dst_port;
    memcpy(wire+6,data,length);
    struct sockaddr_in client={.sin_family=AF_INET,.sin_port=htons(m->tuple.src_port),.sin_addr.s_addr=m->tuple.src_ip};
    return sendto(app_socket,wire,length+6,0,(struct sockaddr *)&client,sizeof(client))==(ssize_t)(length+6);
}
/* Optional integration of the production delivery pool into the real select
 * loop. Inline post models a callback completing before post returns. */
static void delivery_lock(void *ctx) { (void)ctx; pthread_mutex_lock(&mutex); }
static void delivery_unlock(void *ctx) { (void)ctx; pthread_mutex_unlock(&mutex); }
static int64_t delivery_now(void *ctx) { (void)ctx; return now_us(); }
static bool delivery_current(void *ctx,uint32_t gen)
{ (void)ctx; return atomic_load(&online) && gen==atomic_load(&generation); }
static bool delivery_post(void *ctx,void (*callback)(void *),void *arg)
{ (void)ctx; callback(arg); return true; }
static udp_emit_result_t delivery_emit(void *ctx,const udp_datagram_t *m)
{
    if (!delivery_current(ctx,m->generation)) return UDP_EMIT_STALE;
    if (rejected_output++<2) return UDP_EMIT_MEM;
    return send_reply(ctx,m,m->bytes,m->tuple.payload_length)?UDP_EMIT_OK:UDP_EMIT_OTHER;
}
static void delivery_wake(void *ctx) { (void)ctx; notify(); }
static int64_t maintain(void *ctx) { (void)ctx; return udp_delivery_pump(&deliveries); }
static bool reply(void *ctx,const udp_datagram_t *m,const uint8_t *data,size_t length)
{
    return delivery_retry?udp_delivery_submit(&deliveries,m,data,length):send_reply(ctx,m,data,length);
}
static void *input(void *arg)
{
    (void)arg;
    while (!atomic_load(&stopping)) {
        struct pollfd fds[2]={{.fd=app_socket,.events=POLLIN},{.fd=STDIN_FILENO,.events=POLLIN}};
        if (poll(fds,2,100)<=0) continue;
        if (fds[1].revents&POLLIN) {
            char command;
            if (read(STDIN_FILENO,&command,1)==1) {
                if (command=='d') { atomic_store(&online,false); atomic_fetch_add(&generation,1); }
                if (command=='u') { atomic_store(&online,true); atomic_fetch_add(&generation,1); }
                if (command=='r') atomic_fetch_add(&generation,1);
                if (command=='q') atomic_store(&stopping,true);
                if (command=='a') proxy_client_disconnect(mac_a);
                if (command=='b') connect_client(mac_a,"127.0.0.2");
                notify();
            }
        }
        if (fds[0].revents&POLLIN) {
            uint8_t wire[PROXY_UDP_MAX_PAYLOAD+7];
            struct sockaddr_in client; socklen_t length=sizeof(client);
            ssize_t n=recvfrom(app_socket,wire,sizeof(wire),0,(struct sockaddr *)&client,&length);
            if (n<6 || n>PROXY_UDP_MAX_PAYLOAD+6) continue;
            if (!atomic_load(&online)) { puts("OFFLINE"); fflush(stdout); continue; }
            udp_datagram_t m={.tuple={.src_ip=client.sin_addr.s_addr,.src_port=ntohs(client.sin_port),
                .dst_port=(uint16_t)wire[4]<<8 | wire[5],.payload_length=(uint16_t)(n-6)},
                .generation=atomic_load(&generation),.queued_us=now_us()};
            if (tracked) m.client_token=proxy_client_token(m.tuple.src_ip);
            memcpy(&m.tuple.dst_ip,wire,4); memcpy(m.bytes,wire+6,n-6);
            unsigned q=m.tuple.dst_port==53?1:0;
            pthread_mutex_lock(&mutex);
            if (size[q]<32) { queue[q][(head[q]+size[q])%32]=m; size[q]++; }
            pthread_mutex_unlock(&mutex); notify();
        }
    }
    return NULL;
}
int main(int argc,char **argv)
{
    if (argc!=3) return 1;
    tracked=getenv("UDP_TEST_CLIENTS")!=NULL;
    if (tracked) { connect_client(mac_a,"127.0.0.2"); connect_client(mac_b,"127.0.0.3"); }
    app_socket=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if (bind(app_socket,(struct sockaddr *)&address,sizeof(address))) return 2;
    socklen_t length=sizeof(address); getsockname(app_socket,(struct sockaddr *)&address,&length);
    wake=eventfd(0,EFD_NONBLOCK);
    if (wake<0) return 3;
    delivery_retry=getenv("UDP_DELIVERY_TEST_RETRY")!=NULL;
    const udp_delivery_ops_t ops={.lock=delivery_lock,.unlock=delivery_unlock,.now_us=delivery_now,
        .current=delivery_current,.post=delivery_post,.emit=delivery_emit,.count=count,.wake=delivery_wake};
    udp_delivery_init(&deliveries,&ops,NULL);
    pthread_t thread; if (pthread_create(&thread,NULL,input,NULL)) return 4;
    printf("%u\n",ntohs(address.sin_port)); fflush(stdout);
    const udp_relay_callbacks_t callbacks={.take=take,.pending=pending,.snapshot=snapshot,
        .reply=reply,.count=count,.stats=stats,.maintain=delivery_retry?maintain:NULL,.stop=stop,
        .client_current=tracked?owner_current:NULL,.client_acquire=tracked?owner_acquire:NULL,
        .client_release=tracked?owner_release:NULL};
    udp_relay_run(argv[1],(uint16_t)atoi(argv[2]),wake,&callbacks,NULL);
    atomic_store(&stopping,true); pthread_join(thread,NULL);
    close(app_socket); close(wake); return 0;
}
