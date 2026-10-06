#include "socks_relay.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "tcp_poc.h"
#include "ap_clients.h"
#include "gateway_state.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
/* Only socket descriptors enter this loop. Avoid the mixed-device VFS select
 * adapter; UDP's eventfd loop still requires that adapter. */
#define relay_select lwip_select
#define REPORT(...) ESP_LOGI("socks_tcp", __VA_ARGS__)
#define DETAIL(...) ESP_LOGD("socks_tcp", __VA_ARGS__)
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <time.h>
#define relay_select select
#define REPORT(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define DETAIL(...) REPORT(__VA_ARGS__)
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }
#endif
#ifndef SOCKS_HANDSHAKE_MS
#define SOCKS_HANDSHAKE_MS 15000
#endif
#ifndef SOCKS_IDLE_MS
#define SOCKS_IDLE_MS 120000
#endif
#ifndef SOCKS_PRESSURE_IDLE_MS
#define SOCKS_PRESSURE_IDLE_MS 30000
#endif

enum { QUEUE_BYTES = 8192 };
typedef struct { unsigned char *bytes; size_t size, offset; } queue_t;
typedef enum { CONNECTING, GREETING_TX, GREETING_RX, REQUEST_TX, REQUEST_RX, RELAY } state_t;
static bool temporary(void) { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR; }
static bool nonblock(int fd) {
    int flags=fcntl(fd,F_GETFL,0), enabled=1;
    return flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0 &&
           setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&enabled,sizeof(enabled))==0;
}
static void abort_socket(int fd) { if (fd >= 0) { struct linger l = {1, 0}; setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)); close(fd); } }
/* Bounded queues keep TCP receive backpressure under lwIP's control. */
static int read_queue(int fd, queue_t *q, bool *eof, uint64_t *count)
{
    int n = recv(fd, q->bytes, QUEUE_BYTES, 0);
    if (n > 0) { q->offset = 0; q->size = n; *count += n; return 1; }
    if (n == 0) { *eof = true; return 1; }
    return temporary() ? 0 : -1;
}
static int write_queue(int fd, queue_t *q)
{
    int n = send(fd, q->bytes + q->offset, q->size, 0);
    if (n > 0) { q->size -= n; q->offset += n; return n; }
    return n < 0 && temporary() ? 0 : -1;
}
static bool shutdown_write(int fd)
{
    /* lwIP can report ERR_CLSD/ERR_CONN after the TCP PCB has closed while
     * received data/EOF remain queued. No more writes are required here;
     * preserve the other direction rather than aborting its pending payload.
     * Actual reset/read/send errors still follow the normal error path. */
    return shutdown(fd,SHUT_WR)==0 || errno==ENOTCONN;
}

#ifndef SOCKS_MAX_SESSIONS
#define SOCKS_MAX_SESSIONS 64
#endif
_Static_assert(SOCKS_MAX_SESSIONS > 0, "at least one TCP session");
typedef struct {
    int client, server, index;
    state_t state;
    queue_t c2s, s2c;
    unsigned char handshake[10], request[10];
    size_t offset;
    bool client_eof, server_eof, client_shutdown, server_shutdown;
    int64_t started, active;
    uint64_t client_bytes, server_bytes;
    int64_t request_sent_ms;
    bool first_response;
    unsigned epoch;
    uint32_t owner;
    uint16_t upstream_local_port;
} session_t;
static const socks_client_ops_t *client_ops;
static bool owner_current(const session_t *c)
{ return !client_ops || client_ops->current(c->owner); }

#if defined(ESP_PLATFORM) && CONFIG_PROXY_PIPELINE_DIAG
typedef struct {
    unsigned count, missing, wnd_zero, rcv_zero, buf_zero, flight_at_limit;
    uint32_t wnd_min, cwnd_min, buf_min, rcv_min, flight_max, unsent_max, unacked_max;
    uint64_t wnd_sum, cwnd_sum, flight_sum;
} pcb_totals_t;
static struct {
    uint64_t calls, wait_us, max_us, queued_calls, queued_us, empty_us;
    unsigned bins[5], timeouts, errors, queue_c2s_peak, queue_s2c_peak;
    unsigned callbacks, request_failed;
    uint64_t callback_us, callback_max_us;
    pcb_totals_t legs[2];
} pipeline_stats;
static bool benchmark_session(const session_t *c,int64_t now)
{
    static const unsigned char peer[4]={192,168,1,10};
    return c->client>=0 && c->state==RELAY &&
        memcmp(c->request+4,peer,4)==0 && c->request[8]==70 && c->request[9]==160 &&
        (c->client_bytes || c->server_bytes) && now-c->active<1000;
}
static void add_pcb(pcb_totals_t *a,const proxy_pcb_sample_t *p)
{
    if (!p->found) { a->missing++; return; }
    if (!a->count) { a->wnd_min=p->snd_wnd; a->cwnd_min=p->cwnd; a->buf_min=p->snd_buf; a->rcv_min=p->rcv_wnd; }
    a->count++;
    if (p->snd_wnd<a->wnd_min) a->wnd_min=p->snd_wnd;
    if (p->cwnd<a->cwnd_min) a->cwnd_min=p->cwnd;
    if (p->snd_buf<a->buf_min) a->buf_min=p->snd_buf;
    if (p->rcv_wnd<a->rcv_min) a->rcv_min=p->rcv_wnd;
    if (p->flight>a->flight_max) a->flight_max=p->flight;
    if (p->unsent>a->unsent_max) a->unsent_max=p->unsent;
    if (p->unacked>a->unacked_max) a->unacked_max=p->unacked;
    a->wnd_zero+=p->snd_wnd==0; a->rcv_zero+=p->rcv_wnd==0; a->buf_zero+=p->snd_buf==0;
    uint32_t limit=p->snd_wnd<p->cwnd?p->snd_wnd:p->cwnd;
    a->flight_at_limit+=limit>0 && p->flight>=limit;
    a->wnd_sum+=p->snd_wnd; a->cwnd_sum+=p->cwnd; a->flight_sum+=p->flight;
}
static void poll_pipeline(void)
{
    proxy_pipeline_sample_t sample;
    if (!tcp_poc_pipeline_poll(&sample)) return;
    pipeline_stats.callbacks++;
    pipeline_stats.callback_us+=sample.callback_wait_us;
    if (sample.callback_wait_us>pipeline_stats.callback_max_us) pipeline_stats.callback_max_us=sample.callback_wait_us;
    add_pcb(&pipeline_stats.legs[0],&sample.client);
    add_pcb(&pipeline_stats.legs[1],&sample.upstream);
}
static void report_pipeline(void)
{
    poll_pipeline();
    if (pipeline_stats.calls || pipeline_stats.callbacks) {
        REPORT("pipeline select calls=%llu wait_us=%llu max_us=%llu bins_lt100us_lt1ms_lt5ms_lt20ms_ge20ms=%u,%u,%u,%u,%u timeout=%u error=%u queued_calls=%llu queued_wait_us=%llu empty_wait_us=%llu c2s_peak=%u s2c_peak=%u",
            (unsigned long long)pipeline_stats.calls,(unsigned long long)pipeline_stats.wait_us,
            (unsigned long long)pipeline_stats.max_us,pipeline_stats.bins[0],pipeline_stats.bins[1],
            pipeline_stats.bins[2],pipeline_stats.bins[3],pipeline_stats.bins[4],pipeline_stats.timeouts,pipeline_stats.errors,
            (unsigned long long)pipeline_stats.queued_calls,(unsigned long long)pipeline_stats.queued_us,
            (unsigned long long)pipeline_stats.empty_us,pipeline_stats.queue_c2s_peak,pipeline_stats.queue_s2c_peak);
        REPORT("pipeline callbacks=%u avg_wait_us=%llu max_wait_us=%llu request_failed=%u",
            pipeline_stats.callbacks,(unsigned long long)(pipeline_stats.callbacks?pipeline_stats.callback_us/pipeline_stats.callbacks:0),
            (unsigned long long)pipeline_stats.callback_max_us,pipeline_stats.request_failed);
        for (unsigned i=0;i<2;i++) {
            pcb_totals_t *a=&pipeline_stats.legs[i];
            REPORT("pipeline pcb leg=%s samples=%u missing=%u wnd_min=%u wnd_avg=%llu cwnd_min=%u cwnd_avg=%llu snd_buf_min=%u rcv_wnd_min=%u flight_avg=%llu flight_max=%u unsent_max=%u unacked_max=%u wnd_zero=%u rcv_zero=%u buf_zero=%u flight_at_limit=%u",
                i?"upstream":"client",a->count,a->missing,a->wnd_min,(unsigned long long)(a->count?a->wnd_sum/a->count:0),
                a->cwnd_min,(unsigned long long)(a->count?a->cwnd_sum/a->count:0),a->buf_min,a->rcv_min,
                (unsigned long long)(a->count?a->flight_sum/a->count:0),a->flight_max,a->unsent_max,a->unacked_max,
                a->wnd_zero,a->rcv_zero,a->buf_zero,a->flight_at_limit);
        }
    }
    memset(&pipeline_stats,0,sizeof(pipeline_stats));
}
#endif
/* Fixed bounded storage: compact hot metadata in internal RAM, bulk payload
 * in PSRAM. Each slot owns two buffers for its entire lifetime; reconnects
 * reset queue metadata without clearing or reallocating 16 KiB of payload. */
typedef unsigned char session_payload_t[2][QUEUE_BYTES];
#ifdef ESP_PLATFORM
static session_t *sessions;
static session_payload_t *payloads;
bool socks_relay_init(void)
{
    if (sessions) return true;
    session_t *metadata = heap_caps_calloc(SOCKS_MAX_SESSIONS, sizeof(*sessions),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!metadata) return false;
    session_payload_t *buffers = heap_caps_malloc(SOCKS_MAX_SESSIONS * sizeof(*payloads),
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffers) { heap_caps_free(metadata); return false; }
    payloads = buffers;
    sessions = metadata;
    REPORT("storage metadata_internal=%u payload_psram=%u queue_bytes=%u",
           (unsigned)(SOCKS_MAX_SESSIONS * sizeof(*sessions)),
           (unsigned)(SOCKS_MAX_SESSIONS * sizeof(*payloads)), (unsigned)QUEUE_BYTES);
    return true;
}
#else
static session_t sessions[SOCKS_MAX_SESSIONS];
static session_payload_t payloads[SOCKS_MAX_SESSIONS];
bool socks_relay_init(void) { return true; }
#endif
static uint64_t to_client, to_upstream; /* Worker-owned cumulative payload bytes. */
static uint64_t finished_ok, finished_reset, responses, response_wait_total_ms, response_wait_max_ms;
static uint64_t idle_closed;
static uint64_t pressure_closed;
static atomic_bool network_ready = ATOMIC_VAR_INIT(true);
static atomic_uint network_epoch = ATOMIC_VAR_INIT(0);
void socks_relay_set_network_ready(bool ready)
{
    if (atomic_exchange(&network_ready, ready) != ready) atomic_fetch_add(&network_epoch, 1);
}
void socks_relay_reset_client(void) { atomic_fetch_add(&network_epoch, 1); }
static void finish(session_t *c, bool failed, socks_retire_fn retire)
{
    if (failed) { abort_socket(c->client); abort_socket(c->server); }
    else { close(c->client); close(c->server); }
    retire(c->index);
    if (client_ops) client_ops->release(c->owner);
    if (failed) finished_reset++; else finished_ok++;
    DETAIL("closed flow=%d upstream_port=%u reset=%d client_bytes=%llu upstream_bytes=%llu", c->index, c->upstream_local_port, failed,
           (unsigned long long)c->client_bytes, (unsigned long long)c->server_bytes);
    c->client = c->server = c->index = -1;
}
static session_t *pressure_candidate(void)
{
    session_t *oldest=NULL;
    int64_t now=now_ms();
    for (unsigned i=0;i<SOCKS_MAX_SESSIONS;i++) {
        session_t *c=&sessions[i];
        /* Do not reclaim handshakes, sessions awaiting their first response,
         * or queued transfers. Demand alone does not interrupt active video. */
        if (c->client<0 || c->state!=RELAY || !c->first_response ||
            c->c2s.size || c->s2c.size || now-c->active<SOCKS_PRESSURE_IDLE_MS) continue;
        if (!oldest || c->active<oldest->active) oldest=c;
    }
    return oldest;
}
static void advance(session_t *c, fd_set *rd, fd_set *wr, fd_set *ex,
                    bool failed, int initial_error, socks_retire_fn retire)
{
        const char *reason=failed?"network_or_select":"none";
        int failure_errno=initial_error;
        bool fresh_c2s=false, fresh_s2c=false;
        if (!failed && (FD_ISSET(c->client, ex) || FD_ISSET(c->server, ex))) {
            int fd=FD_ISSET(c->client,ex)?c->client:c->server;
            socklen_t length=sizeof(failure_errno);
            if (getsockopt(fd,SOL_SOCKET,SO_ERROR,&failure_errno,&length)!=0) failure_errno=errno;
            reason=fd==c->client?"client_exception":"upstream_exception"; failed=true;
        }
        if (!failed && c->state == CONNECTING && FD_ISSET(c->server, wr)) {
            int error = 0; socklen_t length = sizeof(error);
            if (getsockopt(c->server, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error) {
                REPORT("upstream connection error=%d", error); reason="upstream_connect"; failure_errno=error; failed=true;
            } else { c->state = GREETING_TX; c->offset = 0; }
        }
        if (!failed && (c->state == GREETING_TX || c->state == REQUEST_TX) && FD_ISSET(c->server, wr)) {
            const unsigned char greeting[] = {5, 1, 0};
            const unsigned char *data = c->state == GREETING_TX ? greeting : c->request;
            size_t length = c->state == GREETING_TX ? 3 : 10;
            int n = send(c->server, data + c->offset, length - c->offset, 0);
            if (n > 0) {
                c->offset += n;
                if (c->offset == length) { c->state = c->state == GREETING_TX ? GREETING_RX : REQUEST_RX; c->offset = 0; }
            } else if (n == 0 || !temporary()) {
                reason=(c->state==GREETING_TX || c->state==REQUEST_TX)?"handshake_send":"handshake_receive";
                failure_errno=n<0?errno:0; failed=true;
            }
        }
        if (!failed && (c->state == GREETING_RX || c->state == REQUEST_RX) && FD_ISSET(c->server, rd)) {
            size_t length = c->state == GREETING_RX ? 2 : 10;
            int n = recv(c->server, c->handshake + c->offset, length - c->offset, 0);
            if (n > 0) {
                c->offset += n;
                /* Reject unsupported BND ATYP as soon as the reply header arrives. */
                if (c->state == REQUEST_RX && c->offset >= 4 && (c->handshake[0] != 5 || c->handshake[1] != 0 || c->handshake[2] != 0 || c->handshake[3] != 1)) {
                    REPORT("CONNECT rejected VER=%u REP=%u RSV=%u ATYP=%u", c->handshake[0], c->handshake[1], c->handshake[2], c->handshake[3]); reason="CONNECT_reply"; failure_errno=0; failed=true;
                }
                if (!failed && c->offset == length) {
                    if (c->state == GREETING_RX) {
                        if (c->handshake[0] != 5 || c->handshake[1] != 0) { REPORT("NO AUTH rejected"); reason="NO_AUTH"; failure_errno=0; failed=true; }
                        else { c->state = REQUEST_TX; c->offset = 0; }
                    } else {
                        char remote[16]; inet_ntop(AF_INET,c->request+4,remote,sizeof(remote));
                        c->state = RELAY;
#ifdef ESP_PLATFORM
                        gateway_tcp_connected();
#endif
                        DETAIL("SOCKS reply flow=%d upstream_port=%u remote=%s:%u handshake_ms=%lld",c->index,c->upstream_local_port,remote,
                               (unsigned)((unsigned)c->request[8]*256+c->request[9]),
                               (long long)(now_ms()-c->started));
                    }
                }
            } else if (n == 0 || !temporary()) {
                reason=(c->state==GREETING_TX || c->state==REQUEST_TX)?"handshake_send":"handshake_receive";
                failure_errno=n<0?errno:0; failed=true;
            }
        }
        if (!failed && FD_ISSET(c->client, rd)) {
            int rc = read_queue(c->client, &c->c2s, &c->client_eof, &c->client_bytes);
            if (rc < 0) { reason="client_receive"; failure_errno=errno; failed=true; }
            if (rc > 0) c->active = now_ms();
            fresh_c2s=rc>0 && c->c2s.size>0;
        }
        if (!failed && c->state == RELAY && FD_ISSET(c->server, rd) && !c->server_eof && c->s2c.size == 0) {
            int rc = read_queue(c->server, &c->s2c, &c->server_eof, &c->server_bytes);
            if (rc < 0) { reason="upstream_receive"; failure_errno=errno; failed=true; }
            if (rc > 0) c->active = now_ms();
            fresh_s2c=rc>0 && c->s2c.size>0;
            if (fresh_s2c && !c->first_response) {
                c->first_response=true;
#ifdef ESP_PLATFORM
                gateway_tcp_response();
#endif
                if (c->request_sent_ms) {
                    uint64_t wait=(uint64_t)(now_ms()-c->request_sent_ms);
                    responses++; response_wait_total_ms+=wait;
                    if (wait>response_wait_max_ms) response_wait_max_ms=wait;
                }
            }
        }
        /* New data was absent from select's write sets. Try it immediately on
         * nonblocking sockets; EAGAIN retains the queue for the next select.
         * One send per direction/session keeps the loop bounded and fair. */
        if (!failed && c->state == RELAY && c->c2s.size && (fresh_c2s || FD_ISSET(c->server, wr))) {
            int rc = write_queue(c->server, &c->c2s); if (rc < 0) { reason="upstream_send"; failure_errno=errno; failed=true; } if (rc > 0) { c->active = now_ms(); to_upstream += rc; if (client_ops) client_ops->traffic(c->owner,true,rc); if (!c->request_sent_ms) c->request_sent_ms=now_ms(); }
        }
        if (!failed && c->s2c.size && (fresh_s2c || FD_ISSET(c->client, wr))) {
            int rc = write_queue(c->client, &c->s2c); if (rc < 0) { reason="client_send"; failure_errno=errno; failed=true; } if (rc > 0) { c->active = now_ms(); to_client += rc; if (client_ops) client_ops->traffic(c->owner,false,rc); }
        }
        if (!failed && c->state == RELAY) {
            if (c->client_eof && !c->c2s.size && !c->server_shutdown) {
                if (!shutdown_write(c->server)) { reason="upstream_shutdown"; failure_errno=errno; failed=true; }
                c->server_shutdown = true;
            }
            if (c->server_eof && !c->s2c.size && !c->client_shutdown) {
                if (!shutdown_write(c->client)) { reason="client_shutdown"; failure_errno=errno; failed=true; }
                c->client_shutdown = true;
            }
        }
        /* Quiet HTTP/TLS keep-alive expiry should notify both peers with FIN.
         * A stalled transfer still aborts: queued data must never be discarded
         * and reported as a clean EOF. Keep handshake/error resets unchanged. */
        if (!failed && c->state==RELAY && !c->c2s.size && !c->s2c.size &&
            now_ms()-c->active>=SOCKS_IDLE_MS) {
            if ((c->client_shutdown || shutdown_write(c->client)) &&
                (c->server_shutdown || shutdown_write(c->server))) {
                REPORT("idle close flow=%d upstream_port=%u",c->index,c->upstream_local_port);
                idle_closed++; finish(c,false,retire); return;
            }
            reason="idle_shutdown"; failure_errno=errno; failed=true;
        }
        if ((c->state != RELAY && now_ms() - c->started >= SOCKS_HANDSHAKE_MS) || now_ms() - c->active >= SOCKS_IDLE_MS) {
            REPORT("timeout state=%d flow=%d", c->state, c->index); reason=c->state==RELAY?"idle_timeout":"handshake_timeout"; failure_errno=0; failed=true;
        }
        if (failed || (c->client_eof && c->server_eof && !c->c2s.size && !c->s2c.size)) {
            if (failed) {
#ifdef ESP_PLATFORM
                /* Client cancellations and normal idle closes are not proxy-health faults. */
                if (!strncmp(reason,"upstream_",9) || !strcmp(reason,"CONNECT_reply") ||
                    !strcmp(reason,"NO_AUTH") || !strncmp(reason,"handshake_",10)) {
                    int code=!strcmp(reason,"CONNECT_reply")?(c->handshake[1]?1000+c->handshake[1]:3000):
                        !strcmp(reason,"NO_AUTH")?(c->handshake[1]?2000+c->handshake[1]:3000):
                        !strcmp(reason,"handshake_timeout")?-ETIMEDOUT:failure_errno?-failure_errno:-ECONNRESET;
                    gateway_event(GW_TCP_ERROR,code);
                }
#endif
                char remote[16]; inet_ntop(AF_INET,c->request+4,remote,sizeof(remote));
                REPORT("error flow=%d upstream_port=%u operation=%s errno=%d remote=%s:%u state=%d idle_ms=%lld pending_c2s=%u pending_s2c=%u",
                    c->index,c->upstream_local_port,reason,failure_errno,remote,
                    (unsigned)((unsigned)c->request[8]*256+c->request[9]),c->state,
                    (long long)(now_ms()-c->active),(unsigned)c->c2s.size,(unsigned)c->s2c.size);
            }
            finish(c, failed, retire);
        }
}
void socks_relay_run(int listener, const char *upstream, uint16_t port,
                     socks_claim_fn claim, socks_retire_fn retire)
{
    socks_relay_run_clients(listener,upstream,port,claim,retire,NULL);
}
void socks_relay_run_clients(int listener,const char *upstream,uint16_t port,
                            socks_claim_fn claim,socks_retire_fn retire,const socks_client_ops_t *owners)
{
    client_ops=owners;
    if (!socks_relay_init()) { REPORT("session storage allocation failed"); return; }
    for (unsigned i = 0; i < SOCKS_MAX_SESSIONS; i++) sessions[i].client = -1;
    REPORT("TCP event loop active_limit=%u", (unsigned)SOCKS_MAX_SESSIONS);
#if defined(ESP_PLATFORM) && CONFIG_PROXY_PIPELINE_DIAG
    REPORT("pipeline diagnostic peer=192.168.1.10:18080 recent_payload_ms=1000 sample_ms=250; select measures shared worker wall time");
#endif
    int64_t last_stats=now_ms();
    unsigned service_cursor=0;
#if defined(ESP_PLATFORM) && CONFIG_PROXY_PIPELINE_DIAG
    int64_t last_sample=0;
    unsigned sample_cursor=0;
#endif
    for (;;) {
        retire(-1);
        unsigned epoch = atomic_load(&network_epoch);
        bool online = atomic_load(&network_ready);
        fd_set rd, wr, ex;
        FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&ex);
        int maximum = listener;
        bool room = false;
        unsigned active=0, pending=0;
        for (unsigned i = 0; i < SOCKS_MAX_SESSIONS; i++) {
            session_t *c = &sessions[i];
            if (c->client < 0) { room = true; continue; }
            if (!online || c->epoch != epoch || !owner_current(c)) {
                REPORT("network/client reset flow=%d", c->index);
                finish(c, true, retire); room = true; continue;
            }
            active++; if (c->state!=RELAY) pending++;
            FD_SET(c->client, &ex); FD_SET(c->server, &ex);
            if (!c->client_eof && c->c2s.size == 0) FD_SET(c->client, &rd);
            if (c->s2c.size) FD_SET(c->client, &wr);
            if (c->state == CONNECTING || c->state == GREETING_TX || c->state == REQUEST_TX ||
                (c->state == RELAY && c->c2s.size)) FD_SET(c->server, &wr);
            if (c->state == GREETING_RX || c->state == REQUEST_RX ||
                (c->state == RELAY && !c->server_eof && c->s2c.size == 0)) FD_SET(c->server, &rd);
            if (c->client > maximum) maximum = c->client;
            if (c->server > maximum) maximum = c->server;
        }
        if (now_ms()-last_stats>=10000) {
#if defined(ESP_PLATFORM) && CONFIG_PROXY_PIPELINE_DIAG
            report_pipeline();
#endif
#if !defined(ESP_PLATFORM) || CONFIG_PROXY_VERBOSE_DIAGNOSTICS
            unsigned c2s_flows=0,s2c_flows=0,awaiting=0;
            size_t c2s_bytes=0,s2c_bytes=0;
            session_t *blocked=NULL; int64_t oldest_wait=0,now=now_ms();
            for (unsigned i=0;i<SOCKS_MAX_SESSIONS;i++) {
                session_t *c=&sessions[i]; if (c->client<0) continue;
                if (c->c2s.size) { c2s_flows++; c2s_bytes+=c->c2s.size; }
                if (c->s2c.size) { s2c_flows++; s2c_bytes+=c->s2c.size; }
                bool first=c->state==RELAY && c->request_sent_ms && !c->first_response;
                if (first) awaiting++;
                if ((c->c2s.size || c->s2c.size || first || c->state!=RELAY) && now-c->active>oldest_wait) {
                    oldest_wait=now-c->active; blocked=c;
                }
            }
            REPORT("queues c2s_flows=%u bytes=%u s2c_flows=%u bytes=%u awaiting_first=%u oldest_idle_ms=%lld",
                c2s_flows,(unsigned)c2s_bytes,s2c_flows,(unsigned)s2c_bytes,awaiting,(long long)oldest_wait);
            if (blocked && oldest_wait>=3000) {
                char remote[16]; inet_ntop(AF_INET,blocked->request+4,remote,sizeof(remote));
                REPORT("blocked flow=%d upstream_port=%u remote=%s:%u state=%d first=%d c2s=%u s2c=%u idle_ms=%lld",
                    blocked->index,blocked->upstream_local_port,remote,
                    (unsigned)((unsigned)blocked->request[8]*256+blocked->request[9]),blocked->state,
                    blocked->first_response,(unsigned)blocked->c2s.size,(unsigned)blocked->s2c.size,(long long)oldest_wait);
#ifdef ESP_PLATFORM
                tcp_poc_trace_flow(blocked->index,blocked->upstream_local_port);
#endif
            }
            REPORT("stats active=%u handshake=%u limit=%u tx_client=%llu tx_upstream=%llu",active,pending,
                (unsigned)SOCKS_MAX_SESSIONS,(unsigned long long)to_client,(unsigned long long)to_upstream);
            REPORT("closed_ok=%llu reset=%llu first_response_count=%llu avg_ms=%llu max_ms=%llu",
                (unsigned long long)finished_ok,(unsigned long long)finished_reset,(unsigned long long)responses,
                (unsigned long long)(responses?response_wait_total_ms/responses:0),(unsigned long long)response_wait_max_ms);
            REPORT("idle_closed=%llu pressure_closed=%llu pressure_eligible=%d",
                (unsigned long long)idle_closed,(unsigned long long)pressure_closed,
                pressure_candidate()!=NULL);
            responses=response_wait_total_ms=response_wait_max_ms=0;
#ifdef ESP_PLATFORM
            wifi_ap_record_t upstream;
            wifi_sta_list_t clients;
            int upstream_rssi=0, client_rssi=0; unsigned channel=0;
            if (esp_wifi_sta_get_ap_info(&upstream)==ESP_OK) { upstream_rssi=upstream.rssi; channel=upstream.primary; }
            if (esp_wifi_ap_get_sta_list(&clients)==ESP_OK) {
                if (clients.num) {
                    client_rssi=clients.sta[0].rssi;
                    REPORT("AP clients=%u/%u",(unsigned)clients.num,PROXY_AP_MAX_CLIENTS);
                    for (unsigned j=0;j<clients.num;j++) REPORT("AP peer " MACSTR " rssi=%d",MAC2STR(clients.sta[j].mac),clients.sta[j].rssi);
                } else REPORT("AP clients=0/%u",PROXY_AP_MAX_CLIENTS);
            }
            proxy_client_info_t peers[PROXY_AP_MAX_CLIENTS]; proxy_clients_snapshot(peers);
            for (unsigned j=0;j<PROXY_AP_MAX_CLIENTS;j++) if (peers[j].connected) {
                struct in_addr addr={.s_addr=peers[j].ip}; char text[16]; inet_ntop(AF_INET,&addr,text,sizeof(text));
                REPORT("client token=%u IP=%s TCP=%u UDP=%u deny_TCP=%u deny_UDP=%u TCP_up=%llu TCP_down=%llu UDP_sent=%llu UDP_output=%llu",
                    peers[j].token,text,peers[j].tcp,peers[j].udp,peers[j].tcp_denied,peers[j].udp_denied,
                    (unsigned long long)peers[j].tcp_up,(unsigned long long)peers[j].tcp_down,
                    (unsigned long long)peers[j].udp_up,(unsigned long long)peers[j].udp_down);
            }
            REPORT("radio channel=%u sta_rssi=%d client_rssi=%d proxy_stack_free=%u",channel,upstream_rssi,client_rssi,
                   (unsigned)uxTaskGetStackHighWaterMark(NULL));
            wifi_bandwidth_t ap_bw,sta_bw;
            if (esp_wifi_get_bandwidth(WIFI_IF_AP,&ap_bw)==ESP_OK && esp_wifi_get_bandwidth(WIFI_IF_STA,&sta_bw)==ESP_OK)
                REPORT("bandwidth AP=%uMHz STA=%uMHz",ap_bw==WIFI_BW40?40u:20u,sta_bw==WIFI_BW40?40u:20u);
#endif
#else
            responses=response_wait_total_ms=response_wait_max_ms=0;
#endif
            last_stats=now_ms();
        }
        /* Watch a full listener only if an idle session can make room. Keeping
         * an unread, ready listener in select without a candidate would spin.
         * Fresh/active saturation still uses lwIP's bounded listen backlog. */
        if (online && (room || pressure_candidate())) FD_SET(listener, &rd);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
#if defined(ESP_PLATFORM) && CONFIG_PROXY_PIPELINE_DIAG
        poll_pipeline();
        bool bench=false;
        unsigned bench_c2s=0,bench_s2c=0;
        int64_t observed_now=now_ms();
        for (unsigned i=0;i<SOCKS_MAX_SESSIONS;i++) {
            session_t *c=&sessions[i];
            if (benchmark_session(c,observed_now)) { bench=true; bench_c2s+=c->c2s.size; bench_s2c+=c->s2c.size; }
        }
        if (bench && observed_now-last_sample>=250) {
            for (unsigned n=0;n<SOCKS_MAX_SESSIONS;n++) {
                unsigned i=(sample_cursor+n)%SOCKS_MAX_SESSIONS;
                if (!benchmark_session(&sessions[i],observed_now)) continue;
                if (!tcp_poc_pipeline_request(sessions[i].index,sessions[i].upstream_local_port)) pipeline_stats.request_failed++;
                sample_cursor=(i+1)%SOCKS_MAX_SESSIONS; last_sample=observed_now; break;
            }
        }
        int64_t select_started=bench?esp_timer_get_time():0;
#endif
        int selected = relay_select(maximum + 1, &rd, &wr, &ex, &tv);
#if defined(ESP_PLATFORM) && CONFIG_PROXY_PIPELINE_DIAG
        int saved_select_errno=errno;
        if (bench) {
            uint64_t us=(uint64_t)(esp_timer_get_time()-select_started);
            pipeline_stats.calls++; pipeline_stats.wait_us+=us;
            if (us>pipeline_stats.max_us) pipeline_stats.max_us=us;
            pipeline_stats.bins[us<100?0:us<1000?1:us<5000?2:us<20000?3:4]++;
            pipeline_stats.timeouts+=selected==0; pipeline_stats.errors+=selected<0;
            if (bench_c2s || bench_s2c) { pipeline_stats.queued_calls++; pipeline_stats.queued_us+=us; }
            else pipeline_stats.empty_us+=us;
            if (bench_c2s>pipeline_stats.queue_c2s_peak) pipeline_stats.queue_c2s_peak=bench_c2s;
            if (bench_s2c>pipeline_stats.queue_s2c_peak) pipeline_stats.queue_s2c_peak=bench_s2c;
        }
        errno=saved_select_errno;
#endif
        if (selected < 0 && errno == EINTR) continue;
        /* Consume selected descriptors before accept can reuse a closed fd. */
        for (unsigned n = 0; n < SOCKS_MAX_SESSIONS; n++) {
            unsigned i=(service_cursor+n)%SOCKS_MAX_SESSIONS;
            session_t *c = &sessions[i];
            if (c->client >= 0) advance(c, &rd, &wr, &ex,
                selected < 0 || !atomic_load(&network_ready) || c->epoch != atomic_load(&network_epoch) || !owner_current(c),
                selected<0?errno:0, retire);
        }
        service_cursor=(service_cursor+1)%SOCKS_MAX_SESSIONS;
        if (selected >= 0 && FD_ISSET(listener, &rd)) {
            session_t *c = NULL;
            for (unsigned i = 0; i < SOCKS_MAX_SESSIONS; i++)
                if (sessions[i].client < 0) { c = &sessions[i]; break; }
            if (!c && atomic_load(&network_ready)) {
                /* Read/write work above may have refreshed the candidate, so
                 * choose again after consuming every selected descriptor. */
                c=pressure_candidate();
            }
            if (!c) continue;
            int fd = accept(listener, NULL, NULL);
            if (fd < 0) continue;
            uint32_t ip; uint16_t dest_port;
            /* Claim even rejected accepts so their metadata is retired promptly. */
            int slot = claim(fd, &ip, &dest_port);
            if (!c || !atomic_load(&network_ready) || !nonblock(fd) || slot < 0) {
                REPORT("accept rejected: capacity/network/mapping flow=%d", slot);
                abort_socket(fd); if (slot >= 0) retire(slot); continue;
            }
            uint32_t owner=client_ops?client_ops->claim_owner(slot):0;
            if (client_ops && !client_ops->acquire(owner)) {
                REPORT("accept rejected: client quota/stale token=%u flow=%d",owner,slot);
                abort_socket(fd); retire(slot); continue;
            }
            /* Do not evict another idle flow for a rejected/stale claimant. */
            if (c->client>=0) {
                bool clean=(c->client_shutdown || shutdown_write(c->client)) &&
                           (c->server_shutdown || shutdown_write(c->server));
                REPORT("capacity idle close flow=%d idle_ms=%lld clean=%d",
                    c->index,(long long)(now_ms()-c->active),clean);
                pressure_closed++; finish(c,!clean,retire);
            }
            struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
            int upstream_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            bool ok = upstream_fd >= 0 && nonblock(upstream_fd) && inet_pton(AF_INET, upstream, &addr.sin_addr) == 1;
            int rc = ok ? connect(upstream_fd, (struct sockaddr *)&addr, sizeof(addr)) : -1;
            if (!ok || (rc < 0 && errno != EINPROGRESS)) {
                REPORT("upstream connect failed errno=%d", errno);
                if (client_ops) client_ops->release(owner);
                abort_socket(fd); abort_socket(upstream_fd); retire(slot); continue;
            }
            unsigned storage_slot = (unsigned)(c - sessions);
            *c = (session_t){.client = fd, .server = upstream_fd, .index = slot,
                .c2s = {.bytes = payloads[storage_slot][0]},
                .s2c = {.bytes = payloads[storage_slot][1]},
                .state = rc == 0 ? GREETING_TX : CONNECTING,
                .epoch = atomic_load(&network_epoch), .owner=owner, .started = now_ms(), .active = now_ms(),
                .request = {5, 1, 0, 1}};
            memcpy(c->request + 4, &ip, 4);
            c->request[8] = dest_port >> 8; c->request[9] = dest_port;
            struct sockaddr_in local={0}; socklen_t local_length=sizeof(local);
            if (getsockname(upstream_fd,(struct sockaddr *)&local,&local_length)==0)
                c->upstream_local_port=ntohs(local.sin_port);
            DETAIL("accepted flow=%d upstream_port=%u",slot,c->upstream_local_port);
        }
    }
}
