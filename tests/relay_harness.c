#include "socks_relay.h"
#include "ap_clients.h"
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
static bool tracked;
static const uint8_t mac_a[6]={2,0,0,0,0,1},mac_b[6]={2,0,0,0,0,2};
static void connect_client(const uint8_t mac[6],const char *ip)
{ uint32_t addr; inet_pton(AF_INET,ip,&addr); proxy_client_connect(mac); proxy_client_assign(mac,addr); }
static uint32_t owner(int fd)
{ struct sockaddr_in a; socklen_t n=sizeof(a); return getpeername(fd,(struct sockaddr *)&a,&n)?0:proxy_client_token(a.sin_addr.s_addr); }
static bool acquire(uint32_t token) { return proxy_client_acquire(token,false,false); }
static void release(uint32_t token) { proxy_client_release(token,false,false); }
static void traffic(uint32_t token,bool up,unsigned bytes) { proxy_client_traffic(token,false,up,bytes); }
static int claim(int fd, uint32_t *ip, uint16_t *port) {
    if (getenv("RELAY_TEST_SMALL_SEND_BUFFER")) {
        int size=4096;
        if (setsockopt(fd,SOL_SOCKET,SO_SNDBUF,&size,sizeof(size))) return -1;
    }
    inet_pton(AF_INET, "1.2.3.4", ip); *port = 443; return tracked?fd:0;
}
static void retire(int index) { (void)index; }
static void *commands(void *arg) {
    (void)arg;
    int ch;
    while ((ch = getchar()) != EOF) {
        if (ch == 'd') socks_relay_set_network_ready(false);
        if (ch == 'u') socks_relay_set_network_ready(true);
        if (ch == 'r') socks_relay_reset_client();
        if (ch == 'a') proxy_client_disconnect(mac_a);
        if (ch == 'b') connect_client(mac_a,"127.0.0.2");
    }
    return NULL;
}
int main(int argc, char **argv) {
    if (argc != 3) return 1;
    tracked=getenv("RELAY_TEST_CLIENTS")!=NULL;
    if (tracked) { connect_client(mac_a,"127.0.0.2"); connect_client(mac_b,"127.0.0.3"); }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(listener, 1)) return 2;
    socklen_t length=sizeof(addr); getsockname(listener, (struct sockaddr *)&addr, &length);
    fcntl(listener, F_SETFL, O_NONBLOCK);
    printf("%u\n", ntohs(addr.sin_port)); fflush(stdout);
    pthread_t worker;
    if (pthread_create(&worker, NULL, commands, NULL)) return 3;
    const socks_client_ops_t owners={.claim_owner=owner,.current=proxy_client_current,.acquire=acquire,.release=release,.traffic=traffic};
    socks_relay_run_clients(listener, argv[1], atoi(argv[2]), claim, retire,tracked?&owners:NULL);
}
