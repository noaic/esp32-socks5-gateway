#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define GATEWAY_EVENT_CAPACITY 64
typedef enum {
    GW_BOOT, GW_AP_UP, GW_AP_DOWN, GW_STA_LINK, GW_STA_IP, GW_STA_DOWN,
    GW_STA_LOST_IP, GW_SUBNET_CONFLICT, GW_CLIENT_JOIN, GW_CLIENT_LEAVE,
    GW_CONFIG_STAGE, GW_CONFIG_CONFIRM, GW_CONFIG_ROLLBACK, GW_PASSWORD_SET,
    GW_MAINT_START, GW_MAINT_DONE, GW_DIAG_DONE, GW_TCP_ERROR, GW_UDP_ERROR,
    GW_LED_SETTING, GW_LED_ERROR, GW_INTERNET_RESULT, GW_INTERNET_SETTING, GW_RECOVERY_COMMIT
} gateway_event_kind_t;
typedef struct {
    uint64_t first_ms, last_ms;
    uint32_t sequence, count;
    int32_t detail;
    gateway_event_kind_t kind;
} gateway_event_t;
typedef struct {
    bool ap_up, sta_link, sta_ip, subnet_conflict;
    uint64_t tcp_connect_ms, tcp_data_ms, tcp_error_ms, udp_reply_ms, dns_reply_ms, udp_error_ms;
    uint32_t tcp_errors, udp_errors, tcp_error_streak;
    int32_t tcp_error, udp_error, sta_reason;
    uint32_t network_generation, internet_failures;
    uint64_t internet_checked_ms, internet_success_ms;
    bool internet_checking;
} gateway_state_t;
enum { GW_INTERNET_UNKNOWN, GW_INTERNET_CHECKING, GW_INTERNET_PASS, GW_INTERNET_FAIL, GW_INTERNET_EXPIRED };
#define GW_INTERNET_VALID_MS 600000ULL
unsigned gateway_internet_status(const gateway_state_t *state,uint64_t now);
void gateway_internet_invalidate(void);
uint32_t gateway_internet_begin(void);
bool gateway_internet_complete(uint32_t generation,int code);
uint32_t gateway_internet_retry_ms(unsigned failures);
/* No socket/heap/Flash work while locked. Clock and locks also have host adapters. */
void gateway_event(gateway_event_kind_t kind,int32_t detail);
void gateway_state_snapshot(gateway_state_t *out);
size_t gateway_events_snapshot(gateway_event_t *out,size_t capacity);
void gateway_tcp_connected(void);
void gateway_tcp_response(void);
void gateway_udp_reply(bool dns);
uint64_t gateway_state_clock_ms(void);
const char *gateway_event_name(gateway_event_kind_t kind);
unsigned gateway_state_led_color(const gateway_state_t *state,bool enabled,bool sta_configured,uint64_t now);
