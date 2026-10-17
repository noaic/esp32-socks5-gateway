#include "net_diagnostics.h"
#include "proxy_config.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_netif_net_stack.h"
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
#include "esp_timer.h"
#include "esp_private/wifi.h"
#endif
#include "lwip/priv/tcp_priv.h"
#include "lwip/memp.h"

#if MEMP_OVERFLOW_CHECK
#error "PCB allocation observer requires the pinned memp_malloc/free ABI"
#endif

typedef struct {
    atomic_uint mem,buf,other;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    atomic_uint calls;
    atomic_int last_error;
#endif
} tx_counts_t;
static esp_netif_t *ap_handle,*sta_handle;
static tx_counts_t ap_tx,sta_tx;
static atomic_uint ap_udp_attempts;
static atomic_uint udp_pre_arp_missing,udp_pre_arp_ready,udp_driver_called;
static atomic_uint clone_input,clone_output;
static atomic_uint client_rto,upstream_rto,client_fast,upstream_fast;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
static atomic_uint rto_state[2][3]; /* client/upstream x handshake/established/closing */
typedef struct { atomic_uint calls,bytes,rejected,last_us,handed,freed; atomic_int held; } rx_counts_t;
typedef struct { atomic_uint callbacks,unicast_ok,unicast_fail,last_us,short_frame; } done_counts_t;
static rx_counts_t ap_rx,sta_rx;
static atomic_int rx_held;
static atomic_uint rx_peak;
static done_counts_t ap_done,sta_done;
static atomic_uint tx_refs,tx_frees;
static atomic_int tx_held;
static atomic_uint pcb_allocations;
#endif
static atomic_uint pcb_failures,pcb_peak;
static atomic_int pcb_live;

extern esp_err_t __real_esp_netif_transmit_wrap(esp_netif_t *,void *,size_t,void *);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
extern esp_err_t __real_esp_netif_receive(esp_netif_t *,void *,size_t,void *);
extern void __real_esp_netif_free_rx_buffer(void *,void *);
extern void __real_esp_netif_netstack_buf_ref(void *);
extern void __real_esp_netif_netstack_buf_free(void *);
#endif
extern void __real_tcp_rexmit_rto_commit(struct tcp_pcb *);
extern void __real_tcp_rexmit_fast(struct tcp_pcb *);
extern void *__real_memp_malloc(memp_t);
extern void __real_memp_free(memp_t,void *);

void *__wrap_memp_malloc(memp_t type)
{
    void *result=__real_memp_malloc(type);
    if (type==MEMP_TCP_PCB) {
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
        atomic_fetch_add_explicit(&pcb_allocations,1,memory_order_relaxed);
#endif
        if (!result) atomic_fetch_add_explicit(&pcb_failures,1,memory_order_relaxed);
        else {
            unsigned live=(unsigned)atomic_fetch_add_explicit(&pcb_live,1,memory_order_relaxed)+1;
            unsigned peak=atomic_load_explicit(&pcb_peak,memory_order_relaxed);
            while (peak<live && !atomic_compare_exchange_weak_explicit(&pcb_peak,&peak,live,
                   memory_order_relaxed,memory_order_relaxed)) { }
        }
    }
    return result;
}
void __wrap_memp_free(memp_t type,void *memory)
{
    if (type==MEMP_TCP_PCB && memory)
        atomic_fetch_sub_explicit(&pcb_live,1,memory_order_relaxed);
    __real_memp_free(type,memory);
}

/* Link interposition on the pinned v6.1 APIs; never modify bytes, pbuf,
 * return codes or PCB. Existing IRAM transmit path gets an IRAM wrapper. */
esp_err_t IRAM_ATTR __wrap_esp_netif_transmit_wrap(esp_netif_t *netif,void *data,
                                                size_t len,void *pbuf)
{
    tx_counts_t *tx=netif==ap_handle?&ap_tx:netif==sta_handle?&sta_tx:NULL;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    if (tx) atomic_fetch_add_explicit(&tx->calls,1,memory_order_relaxed);
#endif
    if (netif==ap_handle && len>=34 && data) {
        const uint8_t *h=data;
        /* wlanif sends a contiguous Ethernet frame to this exact API. */
        if (h[12]==0x08 && h[13]==0 && (h[14]>>4)==4 && h[23]==17)
            atomic_fetch_add_explicit(&ap_udp_attempts,1,memory_order_relaxed);
    }
    esp_err_t result=__real_esp_netif_transmit_wrap(netif,data,len,pbuf);
    if (tx && result!=ESP_OK) {
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
        atomic_store_explicit(&tx->last_error,result,memory_order_relaxed);
#endif
        atomic_uint *counter=result==ESP_ERR_NO_MEM?&tx->mem:
            result==ESP_ERR_ESP_NETIF_TX_FAILED?&tx->buf:&tx->other;
        atomic_fetch_add_explicit(counter,1,memory_order_relaxed);
    }
    return result;
}

#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
/* These per-packet samples feed detailed UART logs, not the Web counters. */
esp_err_t IRAM_ATTR __wrap_esp_netif_receive(esp_netif_t *netif,void *data,
                                           size_t len,void *driver_buffer)
{
    rx_counts_t *rx=netif==ap_handle?&ap_rx:netif==sta_handle?&sta_rx:NULL;
    if (rx) {
        atomic_fetch_add_explicit(&rx->calls,1,memory_order_relaxed);
        atomic_fetch_add_explicit(&rx->bytes,len,memory_order_relaxed);
        atomic_store_explicit(&rx->last_us,(uint32_t)esp_timer_get_time(),memory_order_relaxed);
        if (driver_buffer) {
            atomic_fetch_add_explicit(&rx->handed,1,memory_order_relaxed);
            atomic_fetch_add_explicit(&rx->held,1,memory_order_relaxed);
            unsigned held=(unsigned)atomic_fetch_add_explicit(&rx_held,1,memory_order_relaxed)+1;
            unsigned peak=atomic_load_explicit(&rx_peak,memory_order_relaxed);
            while (peak<held && !atomic_compare_exchange_weak_explicit(&rx_peak,&peak,held,
                   memory_order_relaxed,memory_order_relaxed)) { }
        }
    }
    /* The real receive path consumes the driver's buffer. Never retain/read it
     * after forwarding, replace the RX callback, or free the buffer here. */
    esp_err_t result=__real_esp_netif_receive(netif,data,len,driver_buffer);
    if (rx && result!=ESP_OK) atomic_fetch_add_explicit(&rx->rejected,1,memory_order_relaxed);
    return result;
}

void IRAM_ATTR __wrap_esp_netif_free_rx_buffer(void *netif,void *buffer)
{
    rx_counts_t *rx=netif==ap_handle?&ap_rx:netif==sta_handle?&sta_rx:NULL;
    if (rx && buffer) {
        atomic_fetch_add_explicit(&rx->freed,1,memory_order_relaxed);
        atomic_fetch_sub_explicit(&rx->held,1,memory_order_relaxed);
        atomic_fetch_sub_explicit(&rx_held,1,memory_order_relaxed);
    }
    /* Original ownership and pointer are unchanged. All pinned wlanif/pbuf
     * free paths call this API. No touching the buffer after this call. */
    __real_esp_netif_free_rx_buffer(netif,buffer);
}

void IRAM_ATTR __wrap_esp_netif_netstack_buf_ref(void *pbuf)
{
    if (pbuf) {
        atomic_fetch_add_explicit(&tx_refs,1,memory_order_relaxed);
        atomic_fetch_add_explicit(&tx_held,1,memory_order_relaxed);
    }
    __real_esp_netif_netstack_buf_ref(pbuf);
}

void IRAM_ATTR __wrap_esp_netif_netstack_buf_free(void *pbuf)
{
    __real_esp_netif_netstack_buf_free(pbuf);
    if (pbuf) {
        atomic_fetch_add_explicit(&tx_frees,1,memory_order_relaxed);
        atomic_fetch_sub_explicit(&tx_held,1,memory_order_relaxed);
    }
}

static void IRAM_ATTR wifi_tx_done(uint8_t ifidx,uint8_t *frame,uint16_t *length,bool success)
{
    done_counts_t *done=ifidx==WIFI_IF_AP?&ap_done:ifidx==WIFI_IF_STA?&sta_done:NULL;
    if (!done) return;
    atomic_fetch_add_explicit(&done->callbacks,1,memory_order_relaxed);
    if (!frame || !length || *length<10) {
        atomic_fetch_add_explicit(&done->short_frame,1,memory_order_relaxed);
        return;
    }
    /* This v6.1 callback supplies an 802.11 frame. Count unicast data only:
     * beacon/multicast and Null/QoS-Null completion must not hide stalled data.
     * Read just FC and receiver address; never modify/retain callback storage. */
    if ((frame[0]&0x0f)==0x08 && !(frame[0]&0x40) && !(frame[4]&1)) {
        atomic_fetch_add_explicit(success?&done->unicast_ok:&done->unicast_fail,1,memory_order_relaxed);
        atomic_store_explicit(&done->last_us,(uint32_t)esp_timer_get_time(),memory_order_relaxed);
    }
}
#endif

esp_err_t net_diagnostics_start_wifi(void)
{
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    /* No other component in this project installs this single observer. The
     * normal esp-netif RX and driver reference/free callbacks stay in place. */
    return esp_wifi_set_tx_done_cb(wifi_tx_done);
#else
    return ESP_OK;
#endif
}
static inline void note_retransmit(const struct tcp_pcb *pcb,bool fast)
{
    atomic_uint *counter=NULL;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    unsigned leg=0;
#endif
    if (pcb->local_port==PROXY_ECHO_PORT) counter=fast?&client_fast:&client_rto;
    else if (pcb->remote_port==PROXY_SOCKS5_PORT) {
        counter=fast?&upstream_fast:&upstream_rto;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
        leg=1;
#endif
    }
    if (counter) {
        atomic_fetch_add_explicit(counter,1,memory_order_relaxed);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
        if (!fast) {
            unsigned state=pcb->state==SYN_SENT || pcb->state==SYN_RCVD?0:pcb->state==ESTABLISHED?1:2;
            atomic_fetch_add_explicit(&rto_state[leg][state],1,memory_order_relaxed);
        }
#endif
    }
}
void IRAM_ATTR __wrap_tcp_rexmit_rto_commit(struct tcp_pcb *pcb)
{
    /* tcp.c's slow timer calls this after preparing an RTO. Intra-tcp_out.c
     * SYN_RCVD recovery does not pass this wrapper: report timer events only. */
    if (pcb) note_retransmit(pcb,false);
    __real_tcp_rexmit_rto_commit(pcb);
}
void IRAM_ATTR __wrap_tcp_rexmit_fast(struct tcp_pcb *pcb)
{
    bool recovering=pcb && (pcb->flags&TF_INFR);
    __real_tcp_rexmit_fast(pcb);
    /* TF_INFR is newly set only after real tcp_rexmit succeeds. No user
     * callback/PCB destruction occurs in this pinned implementation. */
    if (pcb && !recovering && (pcb->flags&TF_INFR)) note_retransmit(pcb,true);
}
void net_diagnostics_init(esp_netif_t *ap,esp_netif_t *sta)
{ ap_handle=ap; sta_handle=sta; }
uint32_t net_diagnostics_ap_udp_attempts(void)
{ return atomic_load_explicit(&ap_udp_attempts,memory_order_relaxed); }
void net_diagnostics_udp_mem(uint32_t before,bool arp_resolved)
{
    /* Other TCPIP callbacks cannot run between this UDP call's snapshots.
     * An ARP request is not a UDP driver attempt. An unresolved neighbour
     * alone does not prove that the three-packet ARP queue was full. */
    atomic_uint *counter=before!=net_diagnostics_ap_udp_attempts()?&udp_driver_called:
        arp_resolved?&udp_pre_arp_ready:&udp_pre_arp_missing;
    atomic_fetch_add_explicit(counter,1,memory_order_relaxed);
}
void net_diagnostics_clone_failed(bool output)
{ atomic_fetch_add_explicit(output?&clone_output:&clone_input,1,memory_order_relaxed); }
#define VALUE(x) atomic_load_explicit(&(x),memory_order_relaxed)
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
static uint32_t sampled_age_ms(unsigned seen,atomic_uint *last_us)
{
    if (!seen) return UINT32_MAX;
    /* Read the callback timestamp before sampling now. Reading a newer
     * timestamp after an older now underflows and falsely reports ~71 min. */
    uint32_t last=VALUE(*last_us);
    uint32_t now=(uint32_t)esp_timer_get_time();
    return (now-last)/1000;
}
#endif
void net_diagnostics_log(void)
{
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    ESP_LOGI("net_diag","tcp_pcb live=%d peak=%u limit=%u allocations=%u failures=%u (includes closing/TIME_WAIT)",
        VALUE(pcb_live),VALUE(pcb_peak),(unsigned)MEMP_NUM_TCP_PCB,VALUE(pcb_allocations),VALUE(pcb_failures));
    ESP_LOGI("net_diag","driver_rx AP calls=%u bytes=%u rejected=%u age_ms=%u STA calls=%u bytes=%u rejected=%u age_ms=%u",
        VALUE(ap_rx.calls),VALUE(ap_rx.bytes),VALUE(ap_rx.rejected),sampled_age_ms(VALUE(ap_rx.calls),&ap_rx.last_us),
        VALUE(sta_rx.calls),VALUE(sta_rx.bytes),VALUE(sta_rx.rejected),sampled_age_ms(VALUE(sta_rx.calls),&sta_rx.last_us));
    ESP_LOGI("net_diag","wifi_done AP callbacks=%u data_ok=%u data_fail=%u age_ms=%u short=%u STA callbacks=%u data_ok=%u data_fail=%u age_ms=%u short=%u",
        VALUE(ap_done.callbacks),VALUE(ap_done.unicast_ok),VALUE(ap_done.unicast_fail),
        sampled_age_ms(VALUE(ap_done.unicast_ok)||VALUE(ap_done.unicast_fail),&ap_done.last_us),VALUE(ap_done.short_frame),
        VALUE(sta_done.callbacks),VALUE(sta_done.unicast_ok),VALUE(sta_done.unicast_fail),
        sampled_age_ms(VALUE(sta_done.unicast_ok)||VALUE(sta_done.unicast_fail),&sta_done.last_us),VALUE(sta_done.short_frame));
    ESP_LOGI("net_diag","driver_pbuf refs=%u frees=%u held=%d",VALUE(tx_refs),VALUE(tx_frees),VALUE(tx_held));
    ESP_LOGI("net_diag","RX_buffers AP handed=%u freed=%u held=%d STA handed=%u freed=%u held=%d total=%d peak=%u dynamic_limit=%u",
        VALUE(ap_rx.handed),VALUE(ap_rx.freed),VALUE(ap_rx.held),
        VALUE(sta_rx.handed),VALUE(sta_rx.freed),VALUE(sta_rx.held),
        VALUE(rx_held),VALUE(rx_peak),(unsigned)CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM);
    ESP_LOGI("net_diag","driver_tx AP calls=%u mem=%u buf=%u other=%u STA calls=%u mem=%u buf=%u other=%u last_error AP=0x%x STA=0x%x",
        VALUE(ap_tx.calls),VALUE(ap_tx.mem),VALUE(ap_tx.buf),VALUE(ap_tx.other),
        VALUE(sta_tx.calls),VALUE(sta_tx.mem),VALUE(sta_tx.buf),VALUE(sta_tx.other),
        (unsigned)VALUE(ap_tx.last_error),(unsigned)VALUE(sta_tx.last_error));
    ESP_LOGI("net_diag","udp_mem before_driver ARP_missing=%u ARP_ready=%u driver_called=%u tcp_clone_fail input=%u output=%u",
        VALUE(udp_pre_arp_missing),VALUE(udp_pre_arp_ready),VALUE(udp_driver_called),VALUE(clone_input),VALUE(clone_output));
    ESP_LOGI("net_diag","tcp_retransmit_events client RTO=%u fast=%u upstream RTO=%u fast=%u (includes SOCKS UDP controls)",
        VALUE(client_rto),VALUE(client_fast),VALUE(upstream_rto),VALUE(upstream_fast));
    ESP_LOGI("net_diag","RTO_state client handshake=%u established=%u closing=%u upstream handshake=%u established=%u closing=%u",
        VALUE(rto_state[0][0]),VALUE(rto_state[0][1]),VALUE(rto_state[0][2]),
        VALUE(rto_state[1][0]),VALUE(rto_state[1][1]),VALUE(rto_state[1][2]));
#endif
}
void net_diagnostics_snapshot(net_diagnostics_snapshot_t *out)
{
    *out=(net_diagnostics_snapshot_t){
        .tcp_pcb_live=VALUE(pcb_live), .tcp_pcb_peak=VALUE(pcb_peak),
        .tcp_pcb_failures=VALUE(pcb_failures),
        .ap_tx_errors=VALUE(ap_tx.mem)+VALUE(ap_tx.buf)+VALUE(ap_tx.other),
        .sta_tx_errors=VALUE(sta_tx.mem)+VALUE(sta_tx.buf)+VALUE(sta_tx.other),
        .clone_failures=VALUE(clone_input)+VALUE(clone_output),
        .client_rto=VALUE(client_rto), .upstream_rto=VALUE(upstream_rto),
        .client_fast=VALUE(client_fast), .upstream_fast=VALUE(upstream_fast),
    };
}
