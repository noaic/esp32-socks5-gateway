#include "sdkconfig.h"
#if CONFIG_PROXY_LINK_BENCH
#include "diagnostic_sta.h"
#include "proxy_config.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

/* Development host reached through the existing LAN SOCKS server. This is
 * STA+SOCKS endpoint throughput, not a direct STA or Internet/VPS benchmark. */
#define PEER_IP "192.168.1.10"
#define PEER_PORT 18080
enum { TOTAL = 8 * 1024 * 1024 };

static bool send_all(int fd, const void *data, size_t length, int64_t deadline)
{
    const unsigned char *bytes = data;
    while (length && esp_timer_get_time() < deadline) {
        int n = send(fd, bytes, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n; length -= n;
    }
    return !length;
}

static bool recv_all(int fd, void *data, size_t length, int64_t deadline)
{
    unsigned char *bytes = data;
    while (length && esp_timer_get_time() < deadline) {
        int n = recv(fd, bytes, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n; length -= n;
    }
    return !length;
}

static int upstream_open(int64_t deadline)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int enabled = 1, flags = fcntl(fd, F_GETFL, 0);
    struct timeval timeout = {.tv_sec = 5};
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(PROXY_SOCKS5_PORT)};
    bool ok = inet_pton(AF_INET, PROXY_SOCKS5_IPV4, &address.sin_addr) == 1 && flags >= 0 &&
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0 &&
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) == 0 &&
        fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    if (ok && connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        ok = errno == EINPROGRESS;
        if (ok) {
            fd_set wr; FD_ZERO(&wr); FD_SET(fd, &wr);
            timeout = (struct timeval){.tv_sec = 5};
            int error = 0; socklen_t len = sizeof(error);
            ok = lwip_select(fd + 1, NULL, &wr, NULL, &timeout) > 0 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0;
        }
    }
    if (ok) ok = fcntl(fd, F_SETFL, flags) == 0;
    unsigned char reply[10], greeting[] = {5, 1, 0}, request[] = {5, 1, 0, 1, 0, 0, 0, 0, PEER_PORT >> 8, PEER_PORT & 255};
    if (ok) ok = inet_pton(AF_INET, PEER_IP, request + 4) == 1 &&
        send_all(fd, greeting, sizeof(greeting), deadline) && recv_all(fd, reply, 2, deadline) &&
        reply[0] == 5 && reply[1] == 0 && send_all(fd, request, sizeof(request), deadline) &&
        recv_all(fd, reply, sizeof(reply), deadline) &&
        reply[0] == 5 && reply[1] == 0 && reply[2] == 0 && reply[3] == 1;
    if (!ok) { close(fd); return -1; }
    return fd;
}

/* Header-only byte reads keep payload boundaries exact without a large buffer.
 * Limited to the controlled Python peer's HTTP/1.x, Content-Length responses. */
static int response_length(int fd, int64_t deadline)
{
    char header[1024]; size_t used = 0;
    while (used < sizeof(header)-1) {
        if (!recv_all(fd, header + used, 1, deadline)) return -1;
        ++used; header[used] = 0;
        if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4)) break;
    }
    if (used < 4 || memcmp(header + used - 4, "\r\n\r\n", 4) ||
        (strncmp(header, "HTTP/1.1 200 ", 13) && strncmp(header, "HTTP/1.0 200 ", 13))) return -1;
    char *length = strstr(header, "\r\nContent-Length: ");
    unsigned value = 0; char tail = 0;
    if (!length || sscanf(length, "\r\nContent-Length: %u%c", &value, &tail) != 2 ||
        tail != '\r' || value > TOTAL || strstr(header, "\r\nTransfer-Encoding:")) return -1;
    return (int)value;
}

static bool transfer(bool upload, const unsigned char *pattern, unsigned char *buffer,
                     size_t block, size_t capacity, double *elapsed_ms, double *setup_ms)
{
    int64_t started = esp_timer_get_time(), deadline = started + 60000000;
    int fd = upstream_open(deadline);
    if (fd < 0) return false;
    *setup_ms = (esp_timer_get_time()-started) / 1000.0;
    char request[256];
    int count = snprintf(request, sizeof(request), upload ?
        "PUT /upload HTTP/1.1\r\nHost: " PEER_IP ":18080\r\nConnection: close\r\nContent-Length: 8388608\r\n\r\n" :
        "GET /8m HTTP/1.1\r\nHost: " PEER_IP ":18080\r\nConnection: close\r\n\r\n");
    bool ok = count > 0 && (size_t)count < sizeof(request) && send_all(fd, request, count, deadline);
    if (ok && upload) {
        for (size_t sent = 0; sent < TOTAL && ok; sent += block)
            ok = send_all(fd, pattern, block, deadline);
        int length = ok ? response_length(fd, deadline) : -1;
        ok = length > 0 && (size_t)length < capacity && recv_all(fd, buffer, length, deadline);
        if (ok) {
            buffer[length] = 0;
            cJSON *reply = cJSON_Parse((char *)buffer);
            const cJSON *bytes = cJSON_GetObjectItemCaseSensitive(reply, "bytes");
            ok = cJSON_IsNumber(bytes) && bytes->valuedouble == TOTAL &&
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(reply, "matches_8m"));
            cJSON_Delete(reply);
        }
    } else if (ok) {
        ok = response_length(fd, deadline) == TOTAL;
        size_t used = 0;
        while (ok && used < TOTAL && esp_timer_get_time() < deadline) {
            size_t want = TOTAL - used; if (want > capacity) want = capacity;
            int n = recv(fd, buffer, want, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || memcmp(buffer, pattern + (used & 255), n)) { ok = false; break; }
            used += n;
        }
        ok = ok && used == TOTAL;
    }
    *elapsed_ms = (esp_timer_get_time()-started) / 1000.0;
    close(fd);
    return ok && *elapsed_ms > 0;
}

static double median(double values[3])
{
    double a = values[0], b = values[1], c = values[2];
    if (a > b) { double t=a; a=b; b=t; }
    if (b > c) { double t=b; b=c; c=t; }
    return a > b ? a : b;
}

esp_err_t diagnostic_sta_run(httpd_req_t *req, const unsigned char *pattern,
                             unsigned char *buffer, size_t block, size_t capacity)
{
    char run[40]; snprintf(run, sizeof(run), "sta-%lld", (long long)esp_timer_get_time());
    ESP_LOGI("link_bench", "BENCH_RESULT {\"run_id\":\"%s\",\"phase\":\"start\",\"streams\":1,\"rounds\":[],\"firmware_label\":\"diag36-sta-socks\"}", run);
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_FAIL;
    cJSON_AddStringToObject(root, "run_id", run);
    cJSON_AddStringToObject(root, "firmware_label", "diag36-sta-socks");
    cJSON_AddNumberToObject(root, "streams", 1);
    cJSON_AddNumberToObject(root, "bytes_per_transfer", TOTAL);
    cJSON *rounds = cJSON_AddArrayToObject(root, "rounds");
    bool ok = rounds != NULL;
    double down[3], up[3];
    for (unsigned i = 0; i < 3 && ok; ++i) {
        double d_ms, u_ms, d_setup, u_setup;
        ok = transfer(false, pattern, buffer, block, capacity, &d_ms, &d_setup) &&
             transfer(true, pattern, buffer, block, capacity, &u_ms, &u_setup);
        if (!ok) break;
        down[i] = TOTAL * 8.0 / d_ms / 1000; up[i] = TOTAL * 8.0 / u_ms / 1000;
        cJSON *round = cJSON_CreateObject();
        if (!round) { ok = false; break; }
        cJSON_AddItemToArray(rounds, round);
        cJSON_AddNumberToObject(round, "down", down[i]); cJSON_AddNumberToObject(round, "up", up[i]);
        cJSON_AddNumberToObject(round, "download_ms", d_ms); cJSON_AddNumberToObject(round, "upload_ms", u_ms);
        cJSON_AddNumberToObject(round, "download_setup_ms", d_setup); cJSON_AddNumberToObject(round, "upload_setup_ms", u_setup);
    }
    cJSON_AddStringToObject(root, "phase", ok ? "complete" : "error");
    if (ok) {
        cJSON_AddNumberToObject(root, "median_down", median(down));
        cJSON_AddNumberToObject(root, "median_up", median(up));
    } else cJSON_AddStringToObject(root, "error", "upstream transfer or content validation failed");
    char *encoded = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!encoded) return ESP_FAIL;
    ESP_LOGI("link_bench", "BENCH_RESULT %s", encoded);
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, encoded);
    cJSON_free(encoded);
    return err;
}
#endif
