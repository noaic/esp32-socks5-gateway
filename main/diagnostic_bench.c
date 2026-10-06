#include "diagnostic_bench.h"
#include "sdkconfig.h"
#if CONFIG_PROXY_LINK_BENCH
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "diagnostic_sta.h"

enum { TOTAL = 8 * 1024 * 1024, BLOCK = 16384, RX = 8192, REPORT_MAX = 4096 };
static const char *TAG = "link_bench";
static unsigned char *pattern, *received;
static httpd_handle_t server;
static struct ifreq ap_interface;
static esp_netif_ip_info_t ap_address;
extern const unsigned char page_start[] asm("_binary_diagnostic_bench_html_start");
extern const unsigned char page_end[] asm("_binary_diagnostic_bench_html_end");

/* Bind to the AP netif and also reject sessions addressed via the STA. */
static esp_err_t ap_session(httpd_handle_t handle, int fd)
{
    (void)handle;
    struct sockaddr_in local, peer;
    socklen_t len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &len) ||
        local.sin_family != AF_INET || local.sin_addr.s_addr != ap_address.ip.addr)
        return ESP_FAIL;
    len = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &len) || peer.sin_family != AF_INET ||
        (peer.sin_addr.s_addr & ap_address.netmask.addr) !=
        (ap_address.ip.addr & ap_address.netmask.addr)) return ESP_FAIL;
    return ESP_OK;
}

static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)page_start, page_end - page_start - 1);
}

static esp_err_t download(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    for (unsigned sent = 0; sent < TOTAL; sent += BLOCK) {
        esp_err_t err = httpd_resp_send_chunk(req, (const char *)pattern, BLOCK);
        if (err != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t invalid(httpd_req_t *req)
{
    httpd_resp_set_status(req, "400 Bad Request");
    return httpd_resp_sendstr(req, "invalid diagnostic request\n");
}

static esp_err_t sta_benchmark(httpd_req_t *req)
{
    if (req->content_len) return invalid(req);
    /* The HTTP server executes one handler at a time: these fixed buffers
     * cannot be touched by AP transfers until the upstream run completes. */
    return diagnostic_sta_run(req, pattern, received, BLOCK, RX);
}

static esp_err_t upload(httpd_req_t *req)
{
    if (req->content_len != TOTAL) return invalid(req);
    size_t offset = 0;
    bool matches = true;
    while (offset < TOTAL) {
        size_t length = TOTAL - offset;
        if (length > RX) length = RX;
        int n = httpd_req_recv(req, (char *)received, length);
        if (n <= 0) return ESP_FAIL; /* Bounded timeout; no endless retry loop. */
        /* Pattern repeats every 256 bytes, even across short receives. */
        if (memcmp(received, pattern + (offset & 255), n)) matches = false;
        offset += n;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, matches ?
        "{\"bytes\":8388608,\"matches_8m\":true}" :
        "{\"bytes\":8388608,\"matches_8m\":false}");
}

/* Bound nesting before cJSON recursion; quoted braces are not structure. */
static bool shallow_json(const char *data, size_t size)
{
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (size_t i = 0; i < size; ++i) {
        char ch = data[i];
        if (!ch) return false;
        if (quoted) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') quoted = false;
        } else if (ch == '"') quoted = true;
        else if (ch == '{' || ch == '[') { if (++depth > 12) return false; }
        else if (ch == '}' || ch == ']') { if (!depth) return false; --depth; }
    }
    return !quoted && !depth;
}

static bool positive_number(const cJSON *item)
{
    return cJSON_IsNumber(item) && isfinite(item->valuedouble) && item->valuedouble > 0;
}

static esp_err_t report(httpd_req_t *req)
{
    if (!req->content_len || req->content_len > REPORT_MAX) return invalid(req);
    size_t used = 0;
    while (used < req->content_len) {
        int n = httpd_req_recv(req, (char *)received + used, req->content_len - used);
        if (n <= 0) return ESP_FAIL;
        used += n;
    }
    received[used] = 0;
    if (!shallow_json((char *)received, used)) return invalid(req);
    cJSON *root = cJSON_ParseWithLengthOpts((char *)received, used + 1, NULL, true);
    const cJSON *phase = cJSON_GetObjectItemCaseSensitive(root, "phase");
    const cJSON *run = cJSON_GetObjectItemCaseSensitive(root, "run_id");
    const cJSON *streams = cJSON_GetObjectItemCaseSensitive(root, "streams");
    const cJSON *rounds = cJSON_GetObjectItemCaseSensitive(root, "rounds");
    bool valid = cJSON_IsObject(root) && cJSON_IsString(phase) && cJSON_IsString(run) &&
        strlen(run->valuestring) > 0 && strlen(run->valuestring) <= 80 &&
        cJSON_IsNumber(streams) && streams->valuedouble == 1 && cJSON_IsArray(rounds) &&
        cJSON_GetArraySize(rounds) <= 3;
    if (valid) {
        valid = !strcmp(phase->valuestring, "start") || !strcmp(phase->valuestring, "round") ||
                !strcmp(phase->valuestring, "complete") || !strcmp(phase->valuestring, "error");
        const cJSON *item;
        cJSON_ArrayForEach(item, rounds) {
            for (unsigned k = 0; k < 4; ++k) {
                static const char *keys[] = {"down", "up", "download_ms", "upload_ms"};
                if (!positive_number(cJSON_GetObjectItemCaseSensitive(item, keys[k]))) valid = false;
            }
        }
        if (!strcmp(phase->valuestring, "complete") && cJSON_GetArraySize(rounds) != 3) valid = false;
    }
    if (!valid) { cJSON_Delete(root); return invalid(req); }
    cJSON_DeleteItemFromObjectCaseSensitive(root, "firmware_label");
    if (!cJSON_AddStringToObject(root, "firmware_label", "diag35-ap-local")) {
        cJSON_Delete(root); return ESP_FAIL;
    }
    char *encoded = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!encoded) return ESP_FAIL;
    /* Only outside transfer timing; escaped JSON avoids log-line injection. */
    ESP_LOGI(TAG, "BENCH_RESULT %s", encoded);
    cJSON_free(encoded);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"saved\":true}");
}

esp_err_t diagnostic_bench_start(esp_netif_t *ap)
{
    if (server) return ESP_OK;
    esp_err_t err = esp_netif_get_ip_info(ap, &ap_address);
    if (err != ESP_OK) return err;
    err = esp_netif_get_netif_impl_name(ap, ap_interface.ifr_name);
    if (err != ESP_OK) return err;
    pattern = heap_caps_malloc(BLOCK + RX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pattern) return ESP_ERR_NO_MEM;
    received = pattern + BLOCK;
    for (unsigned i = 0; i < BLOCK; ++i) pattern[i] = i & 255;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 18081;
    config.core_id = 1; /* Same CPU as current relay; endpoint has no STA leg. */
    config.task_priority = 5;
    config.stack_size = 8192;
    config.max_req_hdr_len = 2048;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = config.send_wait_timeout = 15;
    config.if_name = &ap_interface;
    config.open_fn = ap_session;
    err = httpd_start(&server, &config);
    if (err == ESP_OK) {
        const httpd_uri_t routes[] = {
            {.uri="/bench", .method=HTTP_GET, .handler=page},
            {.uri="/8m", .method=HTTP_GET, .handler=download},
            {.uri="/upload", .method=HTTP_PUT, .handler=upload},
            {.uri="/results", .method=HTTP_POST, .handler=report},
            {.uri="/sta-bench", .method=HTTP_POST, .handler=sta_benchmark},
        };
        for (unsigned i = 0; i < sizeof(routes)/sizeof(routes[0]); ++i) {
            err = httpd_register_uri_handler(server, &routes[i]);
            if (err != ESP_OK) break;
        }
    }
    if (err != ESP_OK) {
        if (server) { httpd_stop(server); server = NULL; }
        heap_caps_free(pattern); pattern = received = NULL;
        return err;
    }
    char ip[16]; struct in_addr address={.s_addr=ap_address.ip.addr};
    inet_ntop(AF_INET,&address,ip,sizeof(ip));
    ESP_LOGI(TAG, "diagnostic AP-only http://%s:18081/bench task_core=1 stack=8192 payload_psram=%u", ip, BLOCK + RX);
    return ESP_OK;
}
#else
esp_err_t diagnostic_bench_start(esp_netif_t *ap) { (void)ap; return ESP_OK; }
#endif
