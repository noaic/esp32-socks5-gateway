#include "tcp_poc.h"
#include "socks_relay.h"
#include "udp_gateway.h"
#include "packet_rewrite.h"
#include "flow_table.h"
#include "proxy_config.h"
#include "ap_clients.h"
#include "net_diagnostics.h"
#include "cpu_profile.h"
#include "link_probe.h"
#include "sdkconfig.h"
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcpip.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/etharp.h"

#if CONFIG_PROXY_POC_ENABLE
#if !CHECKSUM_GEN_TCP || !CHECKSUM_GEN_IP || LWIP_CHECKSUM_CTRL_PER_NETIF
#error "Local output rewrite requires lwIP software IPv4/TCP checksum generation"
#endif
#define SYN 0x02
#define FIN 0x01
#define RST 0x04
#define ACK 0x10

static const char *TAG = "tcp_poc";
static flow_table_t *s_table;
#define s_flows (s_table->flows)
static portMUX_TYPE s_flow_lock = portMUX_INITIALIZER_UNLOCKED;
static ip4_addr_t s_ap_ip;
static ip4_addr_t s_test_ip;
static bool s_enabled;
static int s_listener = -1;
/* Counters only; protocol bytes and lwIP-owned state are unchanged. */
enum { INPUT_OK, INPUT_PARSE, INPUT_MAPPING, OUTPUT_OK, OUTPUT_PARSE,
       OUTPUT_MAPPING, OUTPUT_SHAPE, OUTPUT_NETIF, PATH_COUNT };
static atomic_uint s_path[PATH_COUNT];
static atomic_int s_last_output_error;
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
static struct { atomic_bool busy; int index; uint16_t upstream_port; } s_trace;
#endif

#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS || CONFIG_PROXY_PIPELINE_DIAG
static unsigned segment_bytes(const struct tcp_seg *seg)
{
    unsigned bytes=0;
    for (;seg;seg=seg->next) bytes+=seg->len;
    return bytes;
}
#endif

#if CONFIG_PROXY_PIPELINE_DIAG
/* Only the proxy worker requests/polls. Callback publishes with release;
 * worker acquires before reading and never reuses storage while busy/ready.
 * No UART logging or active probes in TCPIP context. */
static struct {
    atomic_bool busy;
    bool ready;
    int index;
    uint16_t upstream_port;
    int64_t requested_us;
    proxy_pipeline_sample_t sample;
} s_pipeline;
static proxy_pcb_sample_t sample_pcb(const struct tcp_pcb *pcb)
{
    return (proxy_pcb_sample_t){.found=true, .snd_buf=pcb->snd_buf,
        .snd_wnd=pcb->snd_wnd, .cwnd=pcb->cwnd,
        .flight=(uint32_t)(pcb->snd_nxt-pcb->lastack),
        .unsent=segment_bytes(pcb->unsent), .unacked=segment_bytes(pcb->unacked),
        .rcv_wnd=pcb->rcv_wnd};
}
static void pipeline_in_tcpip(void *arg)
{
    (void)arg;
    LWIP_ASSERT_CORE_LOCKED();
    proxy_pipeline_sample_t sample={.callback_wait_us=(uint64_t)(esp_timer_get_time()-s_pipeline.requested_us)};
    flow_t mapping={0};
    portENTER_CRITICAL(&s_flow_lock);
    if (s_table && s_pipeline.index>=0 && s_pipeline.index<PROXY_MAX_TCP_MAPPINGS)
        mapping=s_flows[s_pipeline.index];
    portEXIT_CRITICAL(&s_flow_lock);
    if (mapping.used && mapping.socket_open) {
        for (const struct tcp_pcb *pcb=tcp_active_pcbs; pcb; pcb=pcb->next) {
            if (pcb->local_port==PROXY_ECHO_PORT && pcb->remote_port==mapping.local_peer_port &&
                IP_IS_V4(&pcb->remote_ip) && ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))==mapping.client_ip)
                sample.client=sample_pcb(pcb);
            if (pcb->local_port==s_pipeline.upstream_port && pcb->remote_port==PROXY_SOCKS5_PORT)
                sample.upstream=sample_pcb(pcb);
        }
    }
    s_pipeline.sample=sample;
    s_pipeline.ready=true;
    atomic_store_explicit(&s_pipeline.busy,false,memory_order_release);
}
bool tcp_poc_pipeline_request(int index,uint16_t upstream_local_port)
{
    if (atomic_load_explicit(&s_pipeline.busy,memory_order_acquire) || s_pipeline.ready) return false;
    atomic_store_explicit(&s_pipeline.busy,true,memory_order_relaxed);
    s_pipeline.index=index; s_pipeline.upstream_port=upstream_local_port;
    s_pipeline.requested_us=esp_timer_get_time();
    if (tcpip_try_callback(pipeline_in_tcpip,NULL)==ERR_OK) return true;
    atomic_store_explicit(&s_pipeline.busy,false,memory_order_release);
    return false;
}
bool tcp_poc_pipeline_poll(proxy_pipeline_sample_t *sample)
{
    if (atomic_load_explicit(&s_pipeline.busy,memory_order_acquire) || !s_pipeline.ready) return false;
    *sample=s_pipeline.sample; s_pipeline.ready=false; return true;
}
#endif

#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
static void trace_pcb(const char *leg, const struct tcp_pcb *pcb)
{
    ESP_LOGI(TAG,"pcb %s local=%u peer=%u state=%u snd_buf=%u wnd=%u cwnd=%u unsent=%u unacked=%u qlen=%u rcv_wnd=%u lastack=%u snd_nxt=%u nrtx=%u rto=%d persist=%u",
        leg,pcb->local_port,pcb->remote_port,(unsigned)pcb->state,
        (unsigned)pcb->snd_buf,(unsigned)pcb->snd_wnd,(unsigned)pcb->cwnd,
        segment_bytes(pcb->unsent),segment_bytes(pcb->unacked),pcb->snd_queuelen,
        (unsigned)pcb->rcv_wnd,(unsigned)pcb->lastack,(unsigned)pcb->snd_nxt,
        pcb->nrtx,pcb->rto,pcb->persist_backoff);
}

static void trace_in_tcpip(void *arg)
{
    (void)arg;
    LWIP_ASSERT_CORE_LOCKED();
    int index=s_trace.index; uint16_t upstream_port=s_trace.upstream_port;
    flow_t mapping={0};
    portENTER_CRITICAL(&s_flow_lock);
    if (s_table && index>=0 && index<PROXY_MAX_TCP_MAPPINGS) mapping=s_flows[index];
    portEXIT_CRITICAL(&s_flow_lock);
    if (mapping.used) {
        link_probe_request(mapping.client_ip);
        ESP_LOGI(TAG,"trace flow=%d mapping_open=%u rx_packets=%u tx_attempts=%u quiet_ms=%lld",
            index,mapping.socket_open,(unsigned)mapping.input_packets,(unsigned)mapping.output_packets,
            (long long)((esp_timer_get_time()-mapping.touched_us)/1000));
        bool client_found=false, upstream_found=false;
        for (const struct tcp_pcb *pcb=tcp_active_pcbs;pcb;pcb=pcb->next) {
            if (pcb->local_port==PROXY_ECHO_PORT && pcb->remote_port==mapping.local_peer_port &&
                ip4_addr_get_u32(ip_2_ip4(&pcb->remote_ip))==mapping.client_ip) {
                trace_pcb("client",pcb); client_found=true;
            } else if (upstream_port && pcb->local_port==upstream_port && pcb->remote_port==PROXY_SOCKS5_PORT) {
                trace_pcb("upstream",pcb); upstream_found=true;
            }
        }
        struct netif *ap;
        NETIF_FOREACH(ap) if (ip4_addr_cmp(netif_ip4_addr(ap),&s_ap_ip)) break;
        ip4_addr_t client={.addr=mapping.client_ip};
        struct eth_addr *mac; const ip4_addr_t *ip;
        if (ap && etharp_find_addr(ap,&client,&mac,&ip)>=0)
            ESP_LOGI(TAG,"trace ARP=" MACSTR " PCB client=%u upstream=%u",MAC2STR(mac->addr),client_found,upstream_found);
        else ESP_LOGI(TAG,"trace ARP=unresolved PCB client=%u upstream=%u",client_found,upstream_found);
    } else ESP_LOGI(TAG,"trace flow=%d mapping no longer present",index);
    atomic_store_explicit(&s_trace.busy,false,memory_order_release);
}

void tcp_poc_trace_flow(int index,uint16_t upstream_local_port)
{
    if (atomic_exchange_explicit(&s_trace.busy,true,memory_order_acquire)) return;
    s_trace.index=index; s_trace.upstream_port=upstream_local_port;
    if (tcpip_try_callback(trace_in_tcpip,NULL)!=ERR_OK)
        atomic_store_explicit(&s_trace.busy,false,memory_order_release);
}
#else
void tcp_poc_trace_flow(int index,uint16_t upstream_local_port)
{ (void)index; (void)upstream_local_port; }
#endif
#if !CONFIG_PROXY_TCP_SOCKS5
static uint8_t s_echo_buffer[2048]; /* One fixed socket event loop, bounded pending output. */
#endif

static bool on_ap(struct netif *netif)
{
    bool enabled;
    portENTER_CRITICAL(&s_flow_lock);
    enabled = s_enabled;
    portEXIT_CRITICAL(&s_flow_lock);
    return enabled && ip4_addr_cmp(netif_ip4_addr(netif), &s_ap_ip);
}

#if !CONFIG_PROXY_TCP_SOCKS5
static bool transient_error(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}
#endif

static void log_tuple(const char *label, const flow_t *flow, uint8_t flags)
{
    // inet_ntoa has shared storage; use independent output buffers.
    char client[16], remote[16], local[16];
    gateway_settings_ap_ip(gateway_settings(),local);
    struct in_addr c = {.s_addr = flow->client_ip};
    struct in_addr r = {.s_addr = flow->remote_ip};
    inet_ntop(AF_INET, &c, client, sizeof(client));
    inet_ntop(AF_INET, &r, remote, sizeof(remote));
    ESP_LOGD(TAG, "%s %s:%u -> %s:%u <-> %s:%u peer_tag=%u flags=0x%02x rx=%u tx=%u",
             label, client, flow->client_port, remote, flow->remote_port,
             local, PROXY_ECHO_PORT, flow->local_peer_port, flags, (unsigned)flow->input_packets, (unsigned)flow->output_packets);
}

static void expire_metadata(void)
{
    // Retain mappings after socket close for at least 2*MSL AND a quiet 2*MSL.
    // FIN retransmission and TCP state themselves remain entirely lwIP-owned.
    const int64_t guard = (int64_t)CONFIG_LWIP_TCP_MSL * 2000;
    const int64_t now = esp_timer_get_time();
    static int64_t last_check;
    if (now-last_check<1000000) return;
    last_check=now;
    portENTER_CRITICAL(&s_flow_lock);
    unsigned expired=flow_table_expire(s_table,now,guard);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    unsigned occupied=s_table->used;
#endif
    portEXIT_CRITICAL(&s_flow_lock);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    static int64_t last_stats;
    if (now-last_stats>=10000000) {
        last_stats=now;
        ESP_LOGI(TAG,"metadata=%u/%u expired=%u internal_free=%u largest=%u PSRAM_free=%u",
             occupied,PROXY_MAX_TCP_MAPPINGS,expired,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        proxy_client_info_t clients[PROXY_AP_MAX_CLIENTS]; proxy_clients_snapshot(clients);
        unsigned owned[PROXY_AP_MAX_CLIENTS]={0},unowned=0;
        portENTER_CRITICAL(&s_flow_lock);
        for (unsigned i=0;i<PROXY_MAX_TCP_MAPPINGS;i++) if (s_flows[i].used) {
            bool match=false;
            for (unsigned j=0;j<PROXY_AP_MAX_CLIENTS;j++) if (clients[j].connected && clients[j].token==s_flows[i].client_token) { owned[j]++; match=true; break; }
            unowned+=!match;
        }
        portEXIT_CRITICAL(&s_flow_lock);
        ESP_LOGI(TAG,"metadata_by_client token0=%u used0=%u token1=%u used1=%u retired_clients=%u internal_min_since_boot=%u",
            clients[0].token,owned[0],clients[1].token,owned[1],unowned,
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
        cpu_profile_log();
        net_diagnostics_log();
        ESP_LOGI(TAG,"rewrite input_ok=%u parse_drop=%u mapping_drop=%u output_ok=%u parse_drop=%u mapping_drop=%u shape_drop=%u netif_error=%u last_err=%d",
            atomic_load(&s_path[INPUT_OK]),atomic_load(&s_path[INPUT_PARSE]),atomic_load(&s_path[INPUT_MAPPING]),
            atomic_load(&s_path[OUTPUT_OK]),atomic_load(&s_path[OUTPUT_PARSE]),atomic_load(&s_path[OUTPUT_MAPPING]),
            atomic_load(&s_path[OUTPUT_SHAPE]),atomic_load(&s_path[OUTPUT_NETIF]),atomic_load(&s_last_output_error));
    }
#endif
    if (expired) {
        ESP_LOGD(TAG, "expired %u mapping(s); internal_free=%u largest=%u PSRAM_free=%u", expired,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

static int map_input(const proxy_tcp_packet_t *packet, flow_t *snapshot, bool *created)
{
    const int64_t now = esp_timer_get_time();
    uint32_t token=proxy_client_token(packet->src_ip);
    if (!token) return -1;
    portENTER_CRITICAL(&s_flow_lock);
    int found=flow_table_input_owned(s_table,packet,token,now,snapshot,created);
    portEXIT_CRITICAL(&s_flow_lock);
    return found;
}

err_t proxy_rewrite_ip4_input(struct pbuf **packet, struct netif *inp)
{
    if (!on_ap(inp)) return ERR_OK;
    uint8_t header[20];
    if (pbuf_copy_partial(*packet, header, sizeof(header), 0) != sizeof(header)) return ERR_OK;
#if CONFIG_PROXY_UDP_ENABLE
    if (header[9] == 17) return udp_gateway_input(*packet, inp);
#endif
    if (header[9] != 6) {
        return ERR_OK;
    }
    uint32_t src, dst;
    memcpy(&src, header + 12, 4);
    memcpy(&dst, header + 16, 4);
    #if CONFIG_PROXY_TCP_SOCKS5
    ip4_addr_t destination = {.addr = dst};
    unsigned first = lwip_ntohl(dst) >> 24;
    if (first == 0 || first >= 224 || ip4_addr_isloopback(&destination) ||
        ip4_addr_netcmp(&destination, &s_ap_ip, netif_ip4_netmask(inp))) return ERR_OK;
#else
    if (dst != s_test_ip.addr) return ERR_OK;
#endif
    struct netif *local;
    NETIF_FOREACH(local) {
        if (netif_ip4_addr(local)->addr == dst) return ERR_OK;
    }
    ip4_addr_t client = {.addr = src};
    if (!ip4_addr_netcmp(&client, &s_ap_ip, netif_ip4_netmask(inp)) ||
        ip4_addr_cmp(&client, &s_ap_ip) || ip4_addr_isbroadcast(&client, inp)) {
        return ERR_VAL;
    }
    // Clone even RX buffers: contiguous, privately writable; parse before making a mapping.
    struct pbuf *copy = pbuf_clone(PBUF_RAW, PBUF_RAM, *packet);
    if (!copy) { net_diagnostics_clone_failed(false); return ERR_MEM; }
    proxy_tcp_packet_t parsed;
    if (copy->len != copy->tot_len || !proxy_tcp_parse(copy->payload, copy->tot_len, &parsed)) {
        atomic_fetch_add(&s_path[INPUT_PARSE],1);
        pbuf_free(copy);
        return ERR_VAL; // Includes MF/offset, IP options, bad original TCP checksum.
    }
    flow_t snapshot;
    bool created;
    if (map_input(&parsed, &snapshot, &created) < 0) {
        atomic_fetch_add(&s_path[INPUT_MAPPING],1);
        static int64_t last_warning;
        int64_t now = esp_timer_get_time();
        if ((parsed.flags & (SYN | ACK)) == SYN && now - last_warning > 5000000) {
            ESP_LOGW(TAG, "SYN mapping unavailable: retained/pending quota, capacity or untracked DHCP client");
            last_warning = now;
        }
        pbuf_free(copy);
        return ERR_VAL;
    }
    // Parser already validated this private copy: update checksum by field delta.
    proxy_tcp_rewrite_validated(copy->payload, false, s_ap_ip.addr, PROXY_ECHO_PORT,
                                snapshot.local_peer_port);
    if (created) log_tuple("map SYN", &snapshot, parsed.flags);
    if ((parsed.flags & (FIN | RST)) & ~snapshot.input_logged) {
        log_tuple("input FIN/RST", &snapshot, parsed.flags);
    }
    pbuf_free(*packet);
    *packet = copy;
    atomic_fetch_add(&s_path[INPUT_OK],1);
    return ERR_OK;
}

int proxy_rewrite_ip4_output(struct pbuf *packet, struct netif *out,
                             const ip4_addr_t *dest, err_t *result)
{
    if (!on_ap(out)) return 0;
    uint8_t header[40];
    if (pbuf_copy_partial(packet, header, sizeof(header), 0) != sizeof(header) ||
        header[0] != 0x45 || header[9] != 6) return 0;
    uint32_t src, dst;
    memcpy(&src, header + 12, 4);
    memcpy(&dst, header + 16, 4);
    const uint16_t source_port = (uint16_t)header[20] << 8 | header[21];
    const uint16_t dest_port = (uint16_t)header[22] << 8 | header[23];
    if (src != s_ap_ip.addr || source_port != PROXY_ECHO_PORT) return 0;
    proxy_tcp_packet_t parsed;
    if (!proxy_tcp_parse_local_header(header,sizeof(header),packet->tot_len,&parsed)) {
        atomic_fetch_add(&s_path[OUTPUT_PARSE],1);
        *result=ERR_VAL;
        return 1;
    }
    flow_t snapshot = {0};
    const uint8_t flags = header[33];
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_flow_lock);
    int mapped=flow_table_peer(s_table,dst,dest_port);
    if (mapped>=0) {
        flow_t *f=&s_flows[mapped];
        ++f->output_packets; f->touched_us=now; snapshot=*f;
        f->output_logged |= flags & (SYN | FIN | RST);
    }
    portEXIT_CRITICAL(&s_flow_lock);
    if (!snapshot.used || !proxy_client_current(snapshot.client_token)) {
        atomic_fetch_add(&s_path[OUTPUT_MAPPING],1);
        // Never expose the local proxy endpoint to a client on a missing mapping.
        *result = ERR_VAL;
        return 1;
    }
    // Do not mutate lwIP's retransmission pbuf or an ARP-retained reference.
    struct pbuf *copy = pbuf_clone(PBUF_LINK, PBUF_RAM, packet);
    if (!copy) {
        net_diagnostics_clone_failed(true);
        *result = ERR_MEM;
        return 1;
    }
    if (copy->len != copy->tot_len ||
        (out->mtu && copy->tot_len > out->mtu)) {
        atomic_fetch_add(&s_path[OUTPUT_SHAPE],1);
        *result = ERR_VAL;
    } else {
        // Local lwIP generated TCP/IP checksums before this hook; payload needs no rescan.
        proxy_tcp_rewrite_validated(copy->payload,true,snapshot.remote_ip,snapshot.remote_port,snapshot.client_port);
        *result = out->output(out, copy, dest);
        if (*result==ERR_OK) atomic_fetch_add(&s_path[OUTPUT_OK],1);
        else {
            atomic_fetch_add(&s_path[OUTPUT_NETIF],1);
            atomic_store(&s_last_output_error,*result);
        }
        if ((flags & (SYN | FIN | RST)) & ~snapshot.output_logged) {
            log_tuple("output SYN/FIN/RST", &snapshot, flags);
        }
    }
    pbuf_free(copy); // netif/ARP may retain its own reference to this independent clone.
    return 1;
}

static int claim_flow(const struct sockaddr_in *peer, flow_t *snapshot)
{
    int index = -1;
    portENTER_CRITICAL(&s_flow_lock);
    index=flow_table_peer(s_table,peer->sin_addr.s_addr,ntohs(peer->sin_port));
    if (index>=0 && !flow_table_claim(s_table,index,snapshot)) index=-1;
    portEXIT_CRITICAL(&s_flow_lock);
    return index;
}

static void retire_flow(int index)
{
    if (index < 0) { expire_metadata(); return; }
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&s_flow_lock);
    flow_table_retire(s_table,index,now);
    portEXIT_CRITICAL(&s_flow_lock);
}

static bool make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

#if !CONFIG_PROXY_TCP_SOCKS5
static void echo_loop(void *arg)
{
    (void)arg;
    int client = -1, index = -1;
    size_t pending = 0, sent = 0;
    uint32_t bytes_in = 0, bytes_out = 0;
    bool eof = false;
    int64_t last_activity = 0;
    for (;;) {
        expire_metadata();
        fd_set reads, writes, errors;
        FD_ZERO(&reads); FD_ZERO(&writes); FD_ZERO(&errors);
        FD_SET(s_listener, &reads);
        int maxfd = s_listener;
        if (client >= 0) {
            if (!eof && pending == 0) FD_SET(client, &reads);
            if (pending) FD_SET(client, &writes);
            FD_SET(client, &errors);
            if (client > maxfd) maxfd = client;
        }
        struct timeval timeout = {.tv_sec = 1};
        int selected = select(maxfd + 1, &reads, &writes, &errors, &timeout);
        if (selected < 0) {
            if (errno == EINTR) continue;
            ESP_LOGE(TAG, "select failed errno=%d", errno);
            ESP_ERROR_CHECK(ESP_FAIL);
        }
        if (FD_ISSET(s_listener, &reads)) {
            struct sockaddr_in peer, local;
            socklen_t peer_len = sizeof(peer), local_len = sizeof(local);
            int fd = accept(s_listener, (struct sockaddr *)&peer, &peer_len);
            if (fd >= 0) {
                flow_t snapshot;
                int new_index = -1;
                if (getsockname(fd, (struct sockaddr *)&local, &local_len) == 0 &&
                    local.sin_addr.s_addr == s_ap_ip.addr && make_nonblocking(fd)) {
                    new_index = claim_flow(&peer, &snapshot);
                }
                if (client >= 0 || new_index < 0) {
                    ESP_LOGW(TAG, "reject accept: busy or missing transparent mapping");
                    close(fd);
                    if (new_index >= 0) retire_flow(new_index);
                } else {
                    client = fd; index = new_index;
                    pending = sent = bytes_in = bytes_out = 0;
                    eof = false;
                    last_activity = esp_timer_get_time();
                    log_tuple("accept original destination", &snapshot, 0);
                    // Newly accepted fd was not in select's original sets.
                    FD_CLR(client, &reads); FD_CLR(client, &writes); FD_CLR(client, &errors);
                }
            } else if (!transient_error()) {
                ESP_LOGW(TAG, "accept failed errno=%d", errno);
            }
        }
        bool close_client = false;
        if (client >= 0 && FD_ISSET(client, &errors)) {
            int error = 0;
            socklen_t len = sizeof(error);
            getsockopt(client, SOL_SOCKET, SO_ERROR, &error, &len);
            ESP_LOGW(TAG, "socket error=%d", error);
            close_client = true;
        }
        if (client >= 0 && !close_client && FD_ISSET(client, &reads)) {
            int n = recv(client, s_echo_buffer, sizeof(s_echo_buffer), 0);
            if (n > 0) {
                pending = n; sent = 0; bytes_in += n;
                last_activity = esp_timer_get_time();
            } else if (n == 0) {
                eof = true;
                ESP_LOGI(TAG, "client EOF; drain echo and close");
            } else if (!transient_error()) {
                ESP_LOGW(TAG, "recv failed errno=%d", errno);
                close_client = true;
            }
        }
        if (client >= 0 && !close_client && pending && FD_ISSET(client, &writes)) {
            int n = send(client, s_echo_buffer + sent, pending, 0);
            if (n > 0) {
                pending -= n; sent += n; bytes_out += n;
                last_activity = esp_timer_get_time();
            } else if (n == 0 || !transient_error()) {
                ESP_LOGW(TAG, "send failed errno=%d", errno);
                close_client = true;
            }
        }
        if (client >= 0 && esp_timer_get_time() - last_activity >
                (int64_t)PROXY_ECHO_IDLE_SECONDS * 1000000) {
            ESP_LOGW(TAG, "echo idle timeout");
            close_client = true;
        }
        if (client >= 0 && (close_client || (eof && pending == 0))) {
            close(client); // lwIP owns FIN, retransmission, ACK and TIME_WAIT.
            retire_flow(index);
            ESP_LOGI(TAG, "closed echo bytes_in=%u bytes_out=%u; mapping retained for 2*MSL",
                     (unsigned)bytes_in, (unsigned)bytes_out);
            ESP_LOGI(TAG, "heap internal_free=%u largest=%u PSRAM_free=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            client = index = -1;
        }
    }
}

#endif

#if CONFIG_PROXY_TCP_SOCKS5
static int relay_claim(int fd, uint32_t *ip, uint16_t *port)
{
    expire_metadata();
    struct sockaddr_in peer, local;
    socklen_t plen = sizeof(peer), llen = sizeof(local);
    flow_t snapshot;
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0 ||
        getsockname(fd, (struct sockaddr *)&local, &llen) != 0 ||
        local.sin_addr.s_addr != s_ap_ip.addr) return -1;
    int index = claim_flow(&peer, &snapshot);
    if (index >= 0) {
        *ip = snapshot.remote_ip; *port = snapshot.remote_port;
        log_tuple("SOCKS5 original destination", &snapshot, 0);
    }
    return index;
}
static uint32_t relay_owner(int index)
{
    uint32_t token=0;
    portENTER_CRITICAL(&s_flow_lock);
    if (index>=0 && index<PROXY_MAX_TCP_MAPPINGS && s_flows[index].used) token=s_flows[index].client_token;
    portEXIT_CRITICAL(&s_flow_lock); return token;
}
static bool relay_acquire(uint32_t token) { return proxy_client_acquire(token,false,false); }
static void relay_release(uint32_t token) { proxy_client_release(token,false,false); }
static void relay_traffic(uint32_t token,bool upload,unsigned bytes) { proxy_client_traffic(token,false,upload,bytes); }
static void relay_loop(void *arg)
{
    (void)arg;
    const socks_client_ops_t clients={.claim_owner=relay_owner,.current=proxy_client_current,
        .acquire=relay_acquire,.release=relay_release,.traffic=relay_traffic};
    socks_relay_run_clients(s_listener, PROXY_SOCKS5_IPV4, PROXY_SOCKS5_PORT, relay_claim, retire_flow,&clients);
}
#endif
#if !CONFIG_PROXY_POC_ENABLE
void tcp_poc_trace_flow(int index,uint16_t upstream_local_port)
{ (void)index; (void)upstream_local_port; }
#endif

esp_err_t tcp_poc_start(void)
{
    char ap_ip[16]; gateway_settings_ap_ip(gateway_settings(),ap_ip);
    if (!ip4addr_aton(ap_ip, &s_ap_ip) ||
        !ip4addr_aton(CONFIG_PROXY_POC_TEST_IP, &s_test_ip) ||
        ip4_addr_isany(&s_test_ip) || ip4_addr_ismulticast(&s_test_ip) ||
        ip4_addr_isloopback(&s_test_ip) ||
        ip4_addr_netcmp(&s_test_ip, &s_ap_ip, &(ip4_addr_t){.addr = PP_HTONL(0xffffff00)}) ||
        (lwip_ntohl(s_test_ip.addr) >> 24) >= 240 || (lwip_ntohl(s_test_ip.addr) >> 24) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
#if CONFIG_PROXY_TCP_SOCKS5
    if (!socks_relay_init()) return ESP_ERR_NO_MEM;
#endif
    s_table=heap_caps_malloc(sizeof(*s_table),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!s_table) return ESP_ERR_NO_MEM;
    flow_table_init(s_table);
    s_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s_listener < 0) return ESP_FAIL;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PROXY_ECHO_PORT),
                               .sin_addr.s_addr = s_ap_ip.addr};
    if (bind(s_listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(s_listener, SOCKS_MAX_SESSIONS) != 0 || !make_nonblocking(s_listener)) {
        ESP_LOGE(TAG, "listener setup failed errno=%d", errno);
        close(s_listener); s_listener = -1;
        return ESP_FAIL;
    }
    #if CONFIG_PROXY_TCP_SOCKS5
    TaskFunction_t loop = relay_loop;
#else
    TaskFunction_t loop = echo_loop;
#endif
    if (xTaskCreatePinnedToCore(loop, "proxy_tcp", 8192, NULL, 5, NULL, 1) != pdPASS) {
        close(s_listener); s_listener = -1;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_flow_lock);
    s_enabled = true;
    portEXIT_CRITICAL(&s_flow_lock);
#if CONFIG_PROXY_TCP_SOCKS5
    ESP_LOGI(TAG, "Phase 3 TCP SOCKS5: upstream=%s:%u NO AUTH IPv4; active=%u metadata=%u",
             PROXY_SOCKS5_IPV4, PROXY_SOCKS5_PORT, (unsigned)SOCKS_MAX_SESSIONS, PROXY_MAX_TCP_MAPPINGS);
#else
    ESP_LOGI(TAG, "Phase 2 TCP echo: test IPv4=%s any TCP port -> local=%u; metadata=%u active_echo=1",
             CONFIG_PROXY_POC_TEST_IP, PROXY_ECHO_PORT, PROXY_MAX_TCP_MAPPINGS);
#endif
    return ESP_OK;
}
#endif
