#include "udp_gateway.h"
#include "udp_relay.h"
#include "proxy_config.h"
#include "ap_clients.h"
#include "socks_relay.h"
#include "net_diagnostics.h"
#include "gateway_state.h"
#include "sdkconfig.h"
#if CONFIG_PROXY_UDP_ENABLE
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include "esp_vfs_eventfd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "lwip/inet.h"
#include "lwip/ip4.h"
#include "lwip/inet_chksum.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"

#define DELIVERY_DEPTH 8
#define DNS_DELIVERY_RESERVED 2
#define INCOMING_DEPTH 16
#define DNS_INCOMING_DEPTH 8
#define PAYLOAD PROXY_UDP_MAX_PAYLOAD
static const char *TAG="udp_proxy";
static atomic_uint diagnostics[UDP_DIAG_COUNT];
static atomic_int last_output_error;
static void count_diag(udp_diag_t counter) { atomic_fetch_add_explicit(&diagnostics[counter],1,memory_order_relaxed); }
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
static unsigned diag(udp_diag_t counter) { return atomic_load_explicit(&diagnostics[counter],memory_order_relaxed); }
#endif
typedef struct { bool busy; udp_datagram_t message; } delivery_t;
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
static bool sta_ready;
static uint32_t generation;
static QueueHandle_t incoming, dns_incoming;
static StaticQueue_t queue_control, dns_queue_control;
static int wake_fd=-1;
static udp_datagram_t *scratch;
static delivery_t *deliveries;
static uint8_t *input_wire;

/* No netconn/socket operation from TCPIP context; VFS eventfd wakes select. */
static void notify_worker(void)
{
    if (wake_fd>=0) { const uint64_t signal=1; (void)write(wake_fd,&signal,sizeof(signal)); }
}
static void put16(uint8_t *p,uint16_t value) { p[0]=value>>8; p[1]=value; }
static bool current(uint32_t gen)
{
    bool ok; portENTER_CRITICAL(&lock); ok=sta_ready && gen==generation; portEXIT_CRITICAL(&lock); return ok;
}
void udp_gateway_reset_client(void)
{
    portENTER_CRITICAL(&lock); ++generation; portEXIT_CRITICAL(&lock);
    notify_worker();
}
void udp_gateway_set_sta_ready(bool ready)
{
    portENTER_CRITICAL(&lock);
    if (sta_ready!=ready) { sta_ready=ready; ++generation; }
    portEXIT_CRITICAL(&lock);
    notify_worker();
}
err_t udp_gateway_input(struct pbuf *packet, struct netif *inp)
{
    uint8_t header[20];
    if (pbuf_copy_partial(packet,header,20,0)!=20) return ERR_OK;
    uint32_t src,dst; memcpy(&src,header+12,4); memcpy(&dst,header+16,4);
    ip4_addr_t client={.addr=src}, remote={.addr=dst};
    unsigned first=lwip_ntohl(dst)>>24;
    if (!first || first>=224 || ip4_addr_isloopback(&remote) ||
        ip4_addr_netcmp(&remote,netif_ip4_addr(inp),netif_ip4_netmask(inp))) return ERR_OK;
    struct netif *local;
    NETIF_FOREACH(local) { if (netif_ip4_addr(local)->addr==dst) return ERR_OK; }
    if (!incoming || !ip4_addr_netcmp(&client,netif_ip4_addr(inp),netif_ip4_netmask(inp)) ||
        ip4_addr_cmp(&client,netif_ip4_addr(inp)) || ip4_addr_isbroadcast(&client,inp)) return ERR_VAL;
    count_diag(UDP_RX);
    if (packet->tot_len>PAYLOAD+28) { count_diag(UDP_TOO_BIG); return ERR_VAL; }
    if (packet->tot_len<28 ||
        pbuf_copy_partial(packet,input_wire,packet->tot_len,0)!=packet->tot_len ||
        !proxy_udp_parse(input_wire,packet->tot_len,&scratch->tuple)) { count_diag(UDP_BAD_INPUT); return ERR_VAL; }
    bool dns=scratch->tuple.dst_port==53;
    if (dns) count_diag(UDP_RX_DNS);
    if (scratch->tuple.dst_port==443) count_diag(UDP_RX_443);
    memcpy(scratch->bytes,input_wire+28,scratch->tuple.payload_length);
    scratch->ap=inp;
    scratch->client_token=proxy_client_token(scratch->tuple.src_ip);
    if (!scratch->client_token) { count_diag(UDP_STALE); return ERR_VAL; }
    portENTER_CRITICAL(&lock); bool ready=sta_ready; scratch->generation=generation; portEXIT_CRITICAL(&lock);
    scratch->queued_us=esp_timer_get_time();
    if (!ready) count_diag(UDP_NOT_READY);
    else if (xQueueSend(dns?dns_incoming:incoming,scratch,0)==pdTRUE) notify_worker();
    else {
        count_diag(UDP_INPUT_FULL);
        count_diag(dns?UDP_INPUT_FULL_DNS:UDP_INPUT_FULL_ORDINARY);
    }
    return ERR_VAL; /* IPv4 caller consumes/frees this intercepted packet. */
}
static void output_error(err_t result)
{
    atomic_store_explicit(&last_output_error,result,memory_order_relaxed);
    switch (result) {
        case ERR_MEM: count_diag(UDP_OUTPUT_MEM); break;
        case ERR_BUF: count_diag(UDP_OUTPUT_BUF); break;
        case ERR_IF: count_diag(UDP_OUTPUT_IF); break;
        default: count_diag(UDP_OUTPUT_OTHER); break;
    }
    count_diag(UDP_OUTPUT_DROP);
}
static void deliver(void *arg)
{
    delivery_t *slot=arg; udp_datagram_t *m=&slot->message;
    struct netif *ap=m->ap;
    if (current(m->generation) && proxy_client_current(m->client_token)) {
        struct pbuf *p=pbuf_alloc(PBUF_IP,m->tuple.payload_length+8,PBUF_RAM);
        if (p && p->len==p->tot_len && netif_is_up(ap)) {
            uint8_t *h=p->payload;
            put16(h,m->tuple.dst_port); put16(h+2,m->tuple.src_port);
            put16(h+4,p->tot_len); put16(h+6,0);
            memcpy(h+8,m->bytes,m->tuple.payload_length);
            ip4_addr_t remote={.addr=m->tuple.dst_ip}, client={.addr=m->tuple.src_ip};
            uint16_t checksum=inet_chksum_pseudo(p,17,p->tot_len,&remote,&client);
            if (!checksum) checksum=0xffff;
            memcpy(h+6,&checksum,2);
            uint32_t before=net_diagnostics_ap_udp_attempts();
            err_t result=ip4_output_if_src(p,&remote,&client,64,0,17,ap);
            if (result==ERR_MEM) {
                struct eth_addr *mac; const ip4_addr_t *ip;
                bool resolved=etharp_find_addr(ap,&client,&mac,&ip)>=0;
                net_diagnostics_udp_mem(before,resolved);
            }
            if (result==ERR_OK) { gateway_udp_reply(m->tuple.dst_port==53); count_diag(UDP_DELIVERED); proxy_client_traffic(m->client_token,true,false,m->tuple.payload_length); }
            else output_error(result);
        } else {
            count_diag(!p?UDP_OUTPUT_ALLOC:!netif_is_up(ap)?UDP_OUTPUT_DOWN:UDP_OUTPUT_OTHER);
            count_diag(UDP_OUTPUT_DROP);
        }
        if (p) pbuf_free(p);
    } else count_diag(UDP_STALE);
    portENTER_CRITICAL(&lock); slot->busy=false; portEXIT_CRITICAL(&lock);
}
static bool queue_reply(void *ctx,const udp_datagram_t *m,const uint8_t *data,size_t length)
{
    (void)ctx;
    if (!current(m->generation) || !proxy_client_current(m->client_token)) { count_diag(UDP_STALE); return false; }
    delivery_t *slot=NULL;
    unsigned limit=m->tuple.dst_port==53?DELIVERY_DEPTH:DELIVERY_DEPTH-DNS_DELIVERY_RESERVED;
    portENTER_CRITICAL(&lock);
    for (unsigned i=0;i<limit;i++) if (!deliveries[i].busy) { slot=&deliveries[i]; slot->busy=true; break; }
    portEXIT_CRITICAL(&lock);
    if (!slot) { count_diag(UDP_DELIVERY_FULL); return false; }
    slot->message.tuple=m->tuple; slot->message.tuple.payload_length=length;
    slot->message.ap=m->ap; slot->message.generation=m->generation;
    slot->message.client_token=m->client_token;
    memcpy(slot->message.bytes,data,length);
    if (tcpip_try_callback(deliver,slot)!=ERR_OK) {
        count_diag(UDP_CALLBACK_FULL);
        portENTER_CRITICAL(&lock); slot->busy=false; portEXIT_CRITICAL(&lock);
        return false;
    }
    return true;
}
static bool take(void *ctx,bool dns,udp_datagram_t *m)
{ (void)ctx; return xQueueReceive(dns?dns_incoming:incoming,m,0)==pdTRUE; }
static bool pending(void *ctx)
{ (void)ctx; return uxQueueMessagesWaiting(dns_incoming) || uxQueueMessagesWaiting(incoming); }
static void snapshot(void *ctx,bool *online,uint32_t *gen)
{
    (void)ctx; portENTER_CRITICAL(&lock); *online=sta_ready; *gen=generation; portEXIT_CRITICAL(&lock);
}
static void count(void *ctx,udp_diag_t key,unsigned amount)
{ (void)ctx; atomic_fetch_add_explicit(&diagnostics[key],amount,memory_order_relaxed); }
static void stats(void *ctx,const udp_relay_stats_t *s)
{
    (void)ctx;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    ESP_LOGI(TAG,"totals flows=%u/%u ready=%u handshake=%u in=%u sent=%u reply=%u output=%u DNS=%u/%u/%u UDP443=%u/%u/%u dns_reclaimed=%u dns_pressure_reclaimed=%u",
        s->active,PROXY_MAX_UDP_FLOWS,s->ready,s->handshake,diag(UDP_RX),diag(UDP_TX),diag(UDP_REPLY),diag(UDP_DELIVERED),
        diag(UDP_RX_DNS),diag(UDP_TX_DNS),diag(UDP_REPLY_DNS),diag(UDP_RX_443),diag(UDP_TX_443),diag(UDP_REPLY_443),diag(UDP_DNS_RECLAIMED),diag(UDP_DNS_PRESSURE_RECLAIMED));
    ESP_LOGI(TAG,"drops big=%u invalid=%u offline=%u queue=%u stale=%u assoc=%u flows=%u socket=%u send=%u recv=%u reply=%u delivery=%u callback=%u output=%u non_dns_limit=%u pending_full=%u pending_expired=%u",
        diag(UDP_TOO_BIG),diag(UDP_BAD_INPUT),diag(UDP_NOT_READY),diag(UDP_INPUT_FULL),diag(UDP_STALE),diag(UDP_ASSOC_LOST),
        diag(UDP_FLOW_FULL),diag(UDP_FLOW_SOCKET),diag(UDP_SEND_DROP),diag(UDP_RECEIVE_DROP),diag(UDP_BAD_REPLY),
        diag(UDP_DELIVERY_FULL),diag(UDP_CALLBACK_FULL),diag(UDP_OUTPUT_DROP),diag(UDP_NON_DNS_LIMIT),diag(UDP_PENDING_FULL),diag(UDP_PENDING_EXPIRED));
    ESP_LOGI(TAG,"local_output failures alloc=%u mem=%u buf=%u if=%u down=%u other=%u last_err=%d retries=%u recovered=%u expired=%u exhausted=%u input_full DNS=%u ordinary=%u",
        diag(UDP_OUTPUT_ALLOC),diag(UDP_OUTPUT_MEM),diag(UDP_OUTPUT_BUF),diag(UDP_OUTPUT_IF),diag(UDP_OUTPUT_DOWN),diag(UDP_OUTPUT_OTHER),
        atomic_load_explicit(&last_output_error,memory_order_relaxed),diag(UDP_OUTPUT_RETRY),diag(UDP_OUTPUT_RECOVERED),
        diag(UDP_DELIVERY_EXPIRED),diag(UDP_DELIVERY_EXHAUSTED),diag(UDP_INPUT_FULL_DNS),diag(UDP_INPUT_FULL_ORDINARY));
    ESP_LOGI(TAG,"dispatches=%llu queue_wait_avg_us=%llu max_us=%llu DNS_RTT count=%llu avg_ms=%llu max_ms=%llu",
        (unsigned long long)s->dispatches,
        (unsigned long long)(s->dispatches?s->queue_wait_total_us/s->dispatches:0),
        (unsigned long long)s->queue_wait_max_us,(unsigned long long)s->dns_replies,
        (unsigned long long)(s->dns_replies?s->dns_rtt_total_ms/s->dns_replies:0),
        (unsigned long long)s->dns_rtt_max_ms);
    ESP_LOGI(TAG,"DNS_wait count=%llu avg_us=%llu max_us=%llu warm_wait count=%llu avg_us=%llu max_us=%llu ASSOC count=%llu avg_us=%llu max_us=%llu",
        (unsigned long long)s->dns_dispatches,
        (unsigned long long)(s->dns_dispatches?s->dns_wait_total_us/s->dns_dispatches:0),
        (unsigned long long)s->dns_wait_max_us,(unsigned long long)s->warm_dispatches,
        (unsigned long long)(s->warm_dispatches?s->warm_wait_total_us/s->warm_dispatches:0),
        (unsigned long long)s->warm_wait_max_us,(unsigned long long)s->associations,
        (unsigned long long)(s->associations?s->association_total_us/s->associations:0),
        (unsigned long long)s->association_max_us);
#else
    (void)s;
#endif
}
static bool client_current(void *ctx,uint32_t token) { (void)ctx; return proxy_client_current(token); }
static bool client_acquire(void *ctx,uint32_t token,bool dns) { (void)ctx; return proxy_client_acquire(token,true,dns); }
static void client_release(void *ctx,uint32_t token,bool dns) { (void)ctx; proxy_client_release(token,true,dns); }
static void client_traffic(void *ctx,uint32_t token,bool upload,size_t bytes)
{ (void)ctx; if (upload) proxy_client_traffic(token,true,true,bytes); }
static void association_error(void *ctx,int code) { (void)ctx;gateway_event(GW_UDP_ERROR,code); }
static void loop(void *arg)
{
    (void)arg;
    const udp_relay_callbacks_t callbacks={.take=take,.pending=pending,.snapshot=snapshot,
        .reply=queue_reply,.count=count,.stats=stats,.association_error=association_error,.client_current=client_current,
        .client_acquire=client_acquire,.client_release=client_release,.client_traffic=client_traffic};
    udp_relay_run(PROXY_SOCKS5_IPV4,PROXY_SOCKS5_PORT,wake_fd,&callbacks,NULL);
    ESP_LOGE(TAG,"UDP worker stopped unexpectedly");
    vTaskDelete(NULL);
}
esp_err_t udp_gateway_start(void)
{
#if !CONFIG_VFS_SUPPORT_SELECT || CONFIG_LWIP_USE_ONLY_LWIP_SELECT
#error "UDP queue wakeup requires VFS mixed socket/eventfd select"
#endif
    bool event_registered=false;
    _Static_assert(PROXY_UDP_DNS_RESERVED>0 && PROXY_UDP_DNS_RESERVED<PROXY_MAX_UDP_FLOWS,"DNS reserve within UDP capacity");
    _Static_assert(CONFIG_LWIP_MAX_UDP_PCBS>=PROXY_MAX_UDP_FLOWS+4,"reserve DHCP/system UDP PCBs");
    _Static_assert(CONFIG_LWIP_MAX_SOCKETS>=2*PROXY_MAX_UDP_FLOWS+2*SOCKS_MAX_SESSIONS+4,"reserve per-flow UDP/control and TCP/system sockets");
    _Static_assert(CONFIG_LWIP_MAX_ACTIVE_TCP>=PROXY_MAX_UDP_FLOWS+2*SOCKS_MAX_SESSIONS+4,"reserve UDP control/TCP PCBs");
    ip4_addr_t upstream;
    if (!ip4addr_aton(PROXY_SOCKS5_IPV4,&upstream) || ip4_addr_isany(&upstream) || ip4_addr_ismulticast(&upstream)) return ESP_ERR_INVALID_ARG;
    uint8_t *storage=heap_caps_malloc(INCOMING_DEPTH*sizeof(udp_datagram_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *dns_storage=heap_caps_malloc(DNS_INCOMING_DEPTH*sizeof(udp_datagram_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    scratch=heap_caps_calloc(1,sizeof(*scratch),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    deliveries=heap_caps_calloc(DELIVERY_DEPTH,sizeof(*deliveries),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    input_wire=heap_caps_malloc(PAYLOAD+28,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!storage || !dns_storage || !scratch || !deliveries || !input_wire || !udp_relay_init()) goto failed;
    incoming=xQueueCreateStatic(INCOMING_DEPTH,sizeof(udp_datagram_t),storage,&queue_control);
    dns_incoming=xQueueCreateStatic(DNS_INCOMING_DEPTH,sizeof(udp_datagram_t),dns_storage,&dns_queue_control);
    if (!incoming || !dns_incoming) goto failed;
    esp_vfs_eventfd_config_t events={.max_fds=1};
    if (esp_vfs_eventfd_register(&events)!=ESP_OK) goto failed;
    event_registered=true;
    wake_fd=eventfd(0,0);
    if (wake_fd<0 || xTaskCreatePinnedToCore(loop,"proxy_udp",6144,NULL,5,NULL,1)!=pdPASS) goto failed;
    ESP_LOGI(TAG,"UDP per-flow association limit=%u queues ordinary=%u DNS=%u delivery=%u DNS_reserved=%u max_payload=%u completed_DNS_idle_s=%u",
        PROXY_MAX_UDP_FLOWS,INCOMING_DEPTH,DNS_INCOMING_DEPTH,DELIVERY_DEPTH,DNS_DELIVERY_RESERVED,PAYLOAD,PROXY_UDP_DNS_IDLE_SECONDS);
    ESP_LOGI(TAG,"local output retry disabled; baseline queues restored, diagnostic counters enabled");
    return ESP_OK;
failed:
    if (wake_fd>=0) { close(wake_fd); wake_fd=-1; }
    if (event_registered) esp_vfs_eventfd_unregister();
    incoming=dns_incoming=NULL;
    heap_caps_free(storage); heap_caps_free(dns_storage); heap_caps_free(scratch);
    heap_caps_free(deliveries); heap_caps_free(input_wire);
    scratch=NULL; deliveries=NULL; input_wire=NULL;
    return ESP_ERR_NO_MEM;
}
#endif
