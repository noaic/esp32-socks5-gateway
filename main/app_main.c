#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "tcp_poc.h"
#include "udp_gateway.h"
#include "proxy_config.h"
#include "ap_clients.h"
#include "socks_relay.h"
#include "net_diagnostics.h"
#include "link_probe.h"
#include "diagnostic_bench.h"
#include "web_management.h"
#include "gateway_state.h"
#include "gateway_control.h"
#include "serial_console.h"
#include "lwip/def.h"

static const char *TAG = "gateway";
static esp_netif_t *s_ap;
static esp_netif_t *s_sta;
static esp_timer_handle_t s_reconnect_timer;
static bool s_ap_started;
static bool s_sta_has_ip;

static void schedule_reconnect(uint64_t delay_us)
{
    esp_err_t err = esp_timer_start_once(s_reconnect_timer, delay_us);
    // A disconnect event and a failed connect request may both schedule a retry.
    if (err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
}

static void log_state(void)
{
    ESP_LOGI(TAG, "state: AP=%s STA_IP=%s ready=%s; internal_free=%u PSRAM_free=%u",
             s_ap_started ? "up" : "down", s_sta_has_ip ? "yes" : "no",
             s_ap_started && s_sta_has_ip ? "yes" : "no",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void connect_sta(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA connect request: %s", esp_err_to_name(err));
        // Retry transient request failures as well as disconnect events.
        schedule_reconnect(5000000);
    }
}

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_AP_START: {
            esp_netif_ip_info_t info;
            s_ap_started = true;
            gateway_event(GW_AP_UP,0);
            ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap, &info));
            ESP_LOGI(TAG, "AP started SSID=%s max_clients=%u IP=" IPSTR,
                     gateway_settings()->ap_ssid, gateway_settings()->max_clients, IP2STR(&info.ip));
            log_state();
            break;
        }
        case WIFI_EVENT_AP_STOP:
            proxy_clients_reset();
#if CONFIG_PROXY_TCP_SOCKS5
            socks_relay_reset_client();
#endif
#if CONFIG_PROXY_UDP_ENABLE
            udp_gateway_reset_client();
#endif
            s_ap_started = false;
            gateway_event(GW_AP_DOWN,0);
            ESP_LOGW(TAG, "AP stopped");
            log_state();
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            const wifi_event_ap_staconnected_t *e = data;
            gateway_event(GW_CLIENT_JOIN,e->aid);
            uint32_t token=proxy_client_connect(e->mac);
            if (!token) ESP_LOGE(TAG,"AP client registry full");
            ESP_LOGI(TAG, "AP client connected " MACSTR " aid=%d", MAC2STR(e->mac), e->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            const wifi_event_ap_stadisconnected_t *e = data;
            gateway_event(GW_CLIENT_LEAVE,e->reason);
            proxy_client_disconnect(e->mac);
            ESP_LOGI(TAG, "AP client disconnected " MACSTR " aid=%d reason=%d",
                     MAC2STR(e->mac), e->aid, e->reason);
            break;
        }
        case WIFI_EVENT_STA_START:
            if (strlen(gateway_settings()->sta_ssid)) {
                ESP_LOGI(TAG, "STA started, connecting to SSID=%s", gateway_settings()->sta_ssid);
                schedule_reconnect(1);
            } else {
                ESP_LOGW(TAG, "STA SSID empty: configure upstream Wi-Fi in AP Web management; AP stays available");
            }
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            const wifi_event_sta_connected_t *e = data;
            gateway_event(GW_STA_LINK,e->channel);
            ESP_LOGI(TAG, "STA connected channel=%u; waiting for DHCP", e->channel);
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *e = data;
            s_sta_has_ip = false;
#if CONFIG_PROXY_TCP_SOCKS5
            socks_relay_set_network_ready(false);
#endif
#if CONFIG_PROXY_UDP_ENABLE
            udp_gateway_set_sta_ready(false);
#endif
            gateway_event(GW_STA_DOWN,e->reason);
            ESP_LOGW(TAG, "STA disconnected reason=%u; retry in 5 seconds", e->reason);
            log_state();
            // Stop is allowed to return INVALID_STATE if the one-shot has fired.
            esp_timer_stop(s_reconnect_timer);
            if (strlen(gateway_settings()->sta_ssid)) {
                schedule_reconnect(5000000);
            }
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_ASSIGNED_IP_TO_CLIENT) {
            const ip_event_assigned_ip_to_client_t *e=data;
            if (e->esp_netif==s_ap) {
                bool accepted=proxy_client_assign(e->mac,e->ip.addr);
                ESP_LOGI(TAG,"AP DHCP " MACSTR " IP=" IPSTR " tracked=%d token=%u",
                         MAC2STR(e->mac),IP2STR(&e->ip),accepted,proxy_client_token(e->ip.addr));
            }
        } else if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = data;
            bool overlap=gateway_settings_ap_overlap(gateway_settings(),
                lwip_ntohl(e->ip_info.ip.addr),lwip_ntohl(e->ip_info.netmask.addr));
            s_sta_has_ip = !overlap;
            gateway_event(overlap?GW_SUBNET_CONFLICT:GW_STA_IP,0);
#if CONFIG_PROXY_TCP_SOCKS5
            socks_relay_set_network_ready(!overlap);
#endif
#if CONFIG_PROXY_UDP_ENABLE
            udp_gateway_set_sta_ready(!overlap);
#endif
            ESP_LOGI(TAG, "STA IP=" IPSTR " mask=" IPSTR " gateway=" IPSTR,
                     IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask), IP2STR(&e->ip_info.gw));
            wifi_ap_record_t upstream;
            if (esp_wifi_sta_get_ap_info(&upstream) == ESP_OK) {
                ESP_LOGI(TAG, "upstream RSSI=%d dBm channel=%u; Wi-Fi PS=NONE", upstream.rssi, upstream.primary);
            }
            if (overlap) ESP_LOGE(TAG,"AP/STA subnets overlap: forwarding paused; change AP subnet in Web management");
            log_state();
        } else if (id == IP_EVENT_STA_LOST_IP) {
            s_sta_has_ip = false;
#if CONFIG_PROXY_TCP_SOCKS5
            socks_relay_set_network_ready(false);
#endif
#if CONFIG_PROXY_UDP_ENABLE
            udp_gateway_set_sta_ready(false);
#endif
            gateway_event(GW_STA_LOST_IP,0);
            ESP_LOGW(TAG, "STA lost IPv4 address");
            log_state();
        }
    }
}

static void verify_hardware(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    ESP_ERROR_CHECK(esp_flash_get_size(NULL, &flash_bytes));
    size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(TAG, "IDF=%s cores=%u Flash=%u bytes PSRAM=%u bytes initialized=%s",
             esp_get_idf_version(), chip.cores, (unsigned)flash_bytes,
             (unsigned)psram_bytes, esp_psram_is_initialized() ? "yes" : "no");
    ESP_ERROR_CHECK(chip.cores == 2 && flash_bytes == 16 * 1024 * 1024 &&
                    esp_psram_is_initialized() && psram_bytes == 8 * 1024 * 1024
                    ? ESP_OK : ESP_ERR_INVALID_SIZE);
    log_state();
}

void app_main(void)
{
    gateway_event(GW_BOOT,esp_reset_reason());
    verify_hardware();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(gateway_settings_init());
    const gateway_settings_t *settings=gateway_settings();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap = esp_netif_create_default_wifi_ap();
    s_sta = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(s_ap && s_sta ? ESP_OK : ESP_ERR_NO_MEM);
    net_diagnostics_init(s_ap,s_sta);
    link_probe_init(s_ap,s_sta);

    // Configure explicitly rather than relying on the default AP address.
    esp_netif_ip_info_t ap_info = {0};
    char ap_ip[16]; gateway_settings_ap_ip(settings,ap_ip);
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(ap_ip, &ap_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(ap_ip, &ap_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4("255.255.255.0", &ap_info.netmask));
    ESP_ERROR_CHECK(esp_netif_dhcps_stop(s_ap));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap, &ap_info));
#if CONFIG_PROXY_UDP_ENABLE
    // Same DNS-offer APIs as the local IDF v6.1 softap_sta example.
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(PROXY_DNS_IPV4, &dns.ip.u_addr.ip4));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns));
    uint8_t offer_dns = 0x02; // DHCPS_OFFER_DNS, value used by official example.
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET,
                    ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
#endif
#if CONFIG_PROXY_UDP_ENABLE
    ESP_LOGI(TAG, "AP DHCP option 6 DNS=%s; gateway=%s; no local DNS server", PROXY_DNS_IPV4,ap_ip);
#endif
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));
    ESP_ERROR_CHECK(esp_netif_set_default_netif(s_sta));

#if CONFIG_PROXY_POC_ENABLE
#if CONFIG_PROXY_TCP_SOCKS5
    socks_relay_set_network_ready(false);
#endif
    // Listener must exist before input rewrite is enabled and Wi-Fi accepts traffic.
    ESP_ERROR_CHECK(tcp_poc_start());
#if CONFIG_PROXY_UDP_ENABLE
    ESP_ERROR_CHECK(udp_gateway_start());
#endif
#endif

    const esp_timer_create_args_t timer_args = {
        .callback = connect_sta,
        .name = "sta_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_LOGI(TAG,"Wi-Fi aggregation RX=%d TX=%d",init.ampdu_rx_enable,init.ampdu_tx_enable);
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    wifi_config_t ap = {
        .ap = {
            .ssid_len = strlen(settings->ap_ssid),
            .channel = CONFIG_PROXY_AP_CHANNEL,
            .max_connection = settings->max_clients,
            .authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    wifi_config_t sta = {
        .sta = {

            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .threshold.authmode = strlen(settings->sta_password) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    memcpy(ap.ap.ssid,settings->ap_ssid,strlen(settings->ap_ssid));
    memcpy(ap.ap.password,settings->ap_password,strlen(settings->ap_password));
    memcpy(sta.sta.ssid,settings->sta_ssid,strlen(settings->sta_ssid));
    memcpy(sta.sta.password,settings->sta_password,strlen(settings->sta_password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    // Restore the IDF b/g/n and negotiated bandwidth defaults: the HT20 and
    // AP b/g compatibility experiments did not resolve the observed stalls.
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_err_t diag_err=net_diagnostics_start_wifi();
    if (diag_err!=ESP_OK) ESP_LOGW(TAG,"Wi-Fi TX completion observer unavailable: %s",esp_err_to_name(diag_err));
    size_t control_before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    esp_err_t control_err=gateway_control_start();
    ESP_LOGI(TAG,"Maintenance startup sampled internal_delta=%d",(int)control_before-(int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    if(control_err!=ESP_OK)ESP_LOGW(TAG,"Maintenance worker unavailable: %s",esp_err_to_name(control_err));
    esp_err_t web_err=web_management_start(s_ap,s_sta);
    if (web_err!=ESP_OK) {
        ESP_LOGE(TAG,"Management unavailable: %s",esp_err_to_name(web_err));
        if (gateway_settings_pending()) esp_restart(); /* Next boot restores confirmed config. */
    }
    // Default is MIN_MODEM in this IDF. Keep the gateway radio awake for recovery.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
#if CONFIG_PROXY_LINK_BENCH
    ESP_ERROR_CHECK(diagnostic_bench_start(s_ap));
#endif
#if CONFIG_PROXY_POC_ENABLE
    ESP_LOGI(TAG, "AP+STA; IPv4 only; forwarding/NAPT disabled; GPIO48 WS2812 status; transparent TCP=%s",
             "enabled");
#else
    ESP_LOGI(TAG, "AP+STA baseline; IPv4 only; transparent TCP=disabled");
#endif
    serial_console_startup_complete();
}
