#include "ap_clients.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
static portMUX_TYPE lock=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&lock)
#define UNLOCK() portEXIT_CRITICAL(&lock)
#else
#include <pthread.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
#endif
_Static_assert(PROXY_AP_MAX_CLIENTS==2,"two-client token layout");
static proxy_client_info_t clients[PROXY_AP_MAX_CLIENTS];
static uint32_t serial;
static uint32_t next_token(unsigned slot)
{
    /* Zero is invalid. Low bit selects the slot; never reset serial on AP stop. */
    serial=(serial+1)&0x7fffffff;
    if (!serial) serial=1;
    return (serial<<1)|slot;
}
static proxy_client_info_t *lookup(uint32_t token)
{
    proxy_client_info_t *c=&clients[token&1];
    return token && c->connected && c->ip && c->token==token?c:NULL;
}
uint32_t proxy_client_connect(const uint8_t mac[6])
{
    LOCK();
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (clients[i].connected && !memcmp(clients[i].mac,mac,6)) {
        uint32_t token=clients[i].token; UNLOCK(); return token;
    }
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (!clients[i].connected) {
        clients[i]=(proxy_client_info_t){.connected=true,.token=next_token(i)};
        memcpy(clients[i].mac,mac,6); uint32_t token=clients[i].token; UNLOCK(); return token;
    }
    UNLOCK(); return 0;
}
bool proxy_client_assign(const uint8_t mac[6],uint32_t ip)
{
    if (!ip) return false;
    LOCK();
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (clients[i].connected && !memcmp(clients[i].mac,mac,6)) {
        for (unsigned j=0;j<PROXY_AP_MAX_CLIENTS;j++) if (j!=i && clients[j].connected && clients[j].ip==ip) {
            UNLOCK(); return false; /* Never silently steal another client's IP. */
        }
        if (clients[i].ip && clients[i].ip!=ip) {
            uint8_t saved[6]; memcpy(saved,mac,6);
            clients[i]=(proxy_client_info_t){.connected=true,.token=next_token(i)};
            memcpy(clients[i].mac,saved,6);
        }
        clients[i].ip=ip; UNLOCK(); return true;
    }
    UNLOCK(); return false;
}
void proxy_client_disconnect(const uint8_t mac[6])
{
    LOCK();
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (!memcmp(clients[i].mac,mac,6)) clients[i].connected=false;
    UNLOCK();
}
void proxy_clients_reset(void)
{
    LOCK(); for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) clients[i].connected=false; UNLOCK();
}
uint32_t proxy_client_token(uint32_t ip)
{
    if (!ip) return 0;
    uint32_t token=0; LOCK();
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (clients[i].connected && clients[i].ip==ip) token=clients[i].token;
    UNLOCK(); return token;
}
bool proxy_client_current(uint32_t token)
{
    LOCK(); bool ok=lookup(token)!=NULL; UNLOCK(); return ok;
}
bool proxy_client_acquire(uint32_t token,bool udp,bool dns)
{
    LOCK(); proxy_client_info_t *c=lookup(token); bool ok=false;
    unsigned present=0;
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) present+=clients[i].connected;
    if (c) {
        if (udp) {
            ok=present<2 || (c->udp<PROXY_CLIENT_UDP_LIMIT && (dns || c->ordinary_udp<PROXY_CLIENT_UDP_LIMIT-2));
            if (ok) { c->udp++; c->ordinary_udp+=!dns; } else c->udp_denied++;
        } else {
            ok=present<2 || c->tcp<PROXY_CLIENT_TCP_LIMIT;
            if (ok) c->tcp++; else c->tcp_denied++;
        }
    }
    UNLOCK(); return ok;
}
void proxy_client_release(uint32_t token,bool udp,bool dns)
{
    LOCK(); proxy_client_info_t *c=lookup(token);
    if (c) {
        if (udp) { if (c->udp) c->udp--; if (!dns && c->ordinary_udp) c->ordinary_udp--; }
        else if (c->tcp) c->tcp--;
    }
    UNLOCK();
}
void proxy_client_traffic(uint32_t token,bool udp,bool upload,uint64_t amount)
{
    LOCK(); proxy_client_info_t *c=lookup(token);
    if (c) {
        uint64_t *counter=udp?(upload?&c->udp_up:&c->udp_down):(upload?&c->tcp_up:&c->tcp_down);
        *counter+=amount;
    }
    UNLOCK();
}
void proxy_clients_snapshot(proxy_client_info_t out[PROXY_AP_MAX_CLIENTS])
{
    LOCK(); memcpy(out,clients,sizeof(clients)); UNLOCK();
}
