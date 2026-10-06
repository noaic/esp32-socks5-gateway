#include "link_probe.h"
#include "sdkconfig.h"
#if CONFIG_PROXY_LINK_PROBE
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

static const char *TAG = "link_probe";
static esp_netif_t *s_ap, *s_sta;
static QueueHandle_t s_requests;
static TaskHandle_t s_worker;
typedef struct { const char *name; uint32_t done_bit; } probe_leg_t;
static probe_leg_t s_legs[] = {{"AP-client", 1}, {"STA-router", 2}};

static void on_reply(esp_ping_handle_t handle, void *arg)
{
    probe_leg_t *leg = arg;
    uint16_t seq = 0;
    uint32_t elapsed = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    ESP_LOGI(TAG, "%s reply seq=%u rtt_ms=%lu", leg->name, seq, (unsigned long)elapsed);
}

static void on_end(esp_ping_handle_t handle, void *arg)
{
    probe_leg_t *leg = arg;
    uint32_t sent = 0, received = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    ESP_LOGI(TAG, "%s finished sent=%lu received=%lu", leg->name,
             (unsigned long)sent, (unsigned long)received);
    xTaskNotify(s_worker, leg->done_bit, eSetBits);
}

static void probe_worker(void *arg)
{
    (void)arg;
    int64_t next_round = 0;
    uint32_t client_ip;
    for (;;) {
        xQueueReceive(s_requests, &client_ip, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        if (now < next_round) continue;
        esp_netif_ip_info_t ap, sta;
        if (esp_netif_get_ip_info(s_ap, &ap) != ESP_OK ||
            esp_netif_get_ip_info(s_sta, &sta) != ESP_OK || !sta.gw.addr ||
            !esp_netif_is_netif_up(s_ap) || !esp_netif_is_netif_up(s_sta)) continue;
        /* A stale flow must never turn this into an Internet probe. */
        uint32_t host = client_ip & ~ap.netmask.addr;
        if ((client_ip & ap.netmask.addr) != (ap.ip.addr & ap.netmask.addr) ||
            client_ip == ap.ip.addr || !host || host == ~ap.netmask.addr ||
            (sta.gw.addr & sta.netmask.addr) != (sta.ip.addr & sta.netmask.addr)) continue;
        next_round = now + 30000000;
        uint32_t targets[] = {client_ip, sta.gw.addr};
        esp_netif_t *interfaces[] = {s_ap, s_sta};
        esp_ping_handle_t handles[2] = {NULL, NULL};
        uint32_t pending = 0;
        /* Clear completed notifications before starting either session. */
        uint32_t notified;
        xTaskNotifyWait(0, UINT32_MAX, &notified, 0);
        for (unsigned i = 0; i < 2; ++i) {
            int index = esp_netif_get_netif_impl_index(interfaces[i]);
            if (index <= 0) continue;
            esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
            config.count = 3;
            config.interval_ms = 500;
            config.timeout_ms = 500;
            config.data_size = 16;
            config.interface = (uint32_t)index;
            IP_SET_TYPE_VAL(config.target_addr, IPADDR_TYPE_V4);
            ip4_addr_set_u32(ip_2_ip4(&config.target_addr), targets[i]);
            esp_ping_callbacks_t callbacks = {
                .cb_args = &s_legs[i], .on_ping_success = on_reply, .on_ping_end = on_end
            };
            char target[16];
            inet_ntop(AF_INET, &targets[i], target, sizeof(target));
            esp_err_t error = esp_ping_new_session(&config, &callbacks, &handles[i]);
            if (error == ESP_OK) error = esp_ping_start(handles[i]);
            ESP_LOGI(TAG, "%s target=%s interface=%d start=%s", s_legs[i].name,
                     target, index, esp_err_to_name(error));
            if (error == ESP_OK) pending |= s_legs[i].done_bit;
            else if (handles[i]) {
                esp_ping_delete_session(handles[i]);
                handles[i] = NULL;
            }
        }
        /* No new round can overlap this one. Callbacks retain static contexts.
         * The official ping task frees its socket/stack asynchronously after
         * delete; the 30-second cooldown exceeds its 1-second exit check. */
        while (pending) {
            xTaskNotifyWait(0, UINT32_MAX, &notified, portMAX_DELAY);
            pending &= ~notified;
        }
        for (unsigned i = 0; i < 2; ++i)
            if (handles[i]) esp_ping_delete_session(handles[i]);
    }
}

void link_probe_init(esp_netif_t *ap, esp_netif_t *sta)
{
    s_ap = ap; s_sta = sta;
    s_requests = xQueueCreate(1, sizeof(uint32_t));
    if (!s_requests) { ESP_LOGW(TAG, "disabled: no queue memory"); return; }
    if (xTaskCreate(probe_worker, "link_probe", 3072, NULL, 2, &s_worker) != pdPASS) {
        vQueueDelete(s_requests); s_requests = NULL;
        ESP_LOGW(TAG, "disabled: no task memory");
    }
}

void link_probe_request(uint32_t client_ip)
{
    if (s_requests) xQueueSend(s_requests, &client_ip, 0);
}
#else
void link_probe_init(esp_netif_t *ap, esp_netif_t *sta)
{ (void)ap; (void)sta; }
void link_probe_request(uint32_t client_ip)
{ (void)client_ip; }
#endif
