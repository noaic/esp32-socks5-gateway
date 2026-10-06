#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Fixed, versioned record. Network settings remain immutable throughout each boot. */
typedef struct {
    char sta_ssid[33], sta_password[65];
    char ap_ssid[33], ap_password[64];
    char socks_ipv4[16], dns_ipv4[16];
    char admin_password[65];
    uint16_t socks_port;
    uint8_t max_clients;
    uint8_t ap_subnet; /* 192.168.X.1/24, X=0..255; formerly v1 trailing padding. */
} gateway_settings_t;
/* Addresses passed to overlap are host-order IPv4 values. */
void gateway_settings_ap_ip(const gateway_settings_t *s,char out[16]);
bool gateway_settings_ap_overlap(const gateway_settings_t *s,uint32_t sta_ip,uint32_t sta_mask);
bool gateway_settings_valid(const gateway_settings_t *s);
#ifdef ESP_PLATFORM
#include "esp_err.h"
const gateway_settings_t *gateway_settings(void);
void gateway_settings_defaults(gateway_settings_t *s);
esp_err_t gateway_settings_init(void);
esp_err_t gateway_settings_stage(const gateway_settings_t *s);
esp_err_t gateway_settings_confirm(void);
esp_err_t gateway_settings_rollback(void);
bool gateway_settings_pending(void);
bool gateway_settings_needs_setup(void);
esp_err_t gateway_settings_setup_password(const char *password);
/* Commit both active/backup, cancel trial; caller must restart on success.
 * network=false: clear admin; true: restore network defaults, keep admin. */
esp_err_t gateway_settings_recover(bool network,char web_ip[16]);
#endif
