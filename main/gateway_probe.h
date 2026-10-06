#pragma once
#include <stdint.h>
typedef struct { int code[3]; uint32_t elapsed_ms[3]; } gateway_probe_result_t;
/* Bounded, no-auth IPv4 SOCKS5 probe. Stages: port, NO AUTH, DNS over UDP ASSOCIATE.
 * 0=success, negative=errno, 1000+REP, 2000+METHOD, 3000=wire invalid,
 * 4000+DNS RCODE, 5000=not executed. No direct DNS fallback. */
void gateway_probe_run(const char *upstream,uint16_t port,const char *dns,
                       gateway_probe_result_t *result,void (*idle)(void));
enum { GATEWAY_INTERNET_TARGETS=2 };
typedef struct {
    gateway_probe_result_t network;
    int http_code;
    uint32_t http_ms, ipv4;
    uint16_t http_status;
} gateway_internet_result_t;
/* Target 0: Microsoft exact short text; target 1: Android HTTP 204.
 * HTTP also uses a NO AUTH IPv4 SOCKS5 CONNECT, never direct fallback.
 * HTTP errors: 6000+status, 7000=unexpected body, otherwise probe codes. */
const char *gateway_internet_target_url(unsigned target);
void gateway_internet_run(const char *upstream,uint16_t port,const char *dns,
                         unsigned target,gateway_internet_result_t *result,void (*idle)(void));
