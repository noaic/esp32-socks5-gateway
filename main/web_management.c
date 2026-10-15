#include "web_management.h"
#include "gateway_settings.h"
#include "management_request.h"
#include "management_http.h"
#include "ap_clients.h"
#include "net_diagnostics.h"
#include "socks_relay.h"
#include "gateway_state.h"
#include "gateway_control.h"
#include "lwip/opt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"

enum { BODY_MAX=1024, CONFIRM_SECONDS=180, HTTP_STACK_BYTES=6144 };
static httpd_handle_t server;
static struct ifreq ap_interface;
static esp_netif_ip_info_t ap_address;
static esp_netif_t *sta_interface;
static esp_timer_handle_t timer;
static bool rebooting;
static uint32_t boot_id;
static int64_t confirmation_deadline;
static char authorization[104], origin[48];
static uint32_t status_peers[PROXY_AP_MAX_CLIENTS];
extern const unsigned char page_start[] asm("_binary_management_html_start");
extern const unsigned char page_end[] asm("_binary_management_html_end");
extern const unsigned char setup_start[] asm("_binary_management_setup_html_start");
extern const unsigned char setup_end[] asm("_binary_management_setup_html_end");

/* Compile-time JSON: stable codes allow browser-side translation without
 * per-request language state or extra response buffers. Keep error for older clients. */
#define API_ERROR(code, message) "{\"error\":\"" message "\",\"error_code\":\"" code "\"}"

static esp_err_t reply(httpd_req_t *req,const char *status,const char *body)
{
    httpd_resp_set_status(req,status);
    httpd_resp_set_type(req,"application/json");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    return httpd_resp_sendstr(req,body);
}
static bool mutation_allowed(httpd_req_t *req)
{
    char value[64];
    if (httpd_req_get_hdr_value_str(req,"X-Proxy-Action",value,sizeof(value))!=ESP_OK ||
        strcmp(value,"manage") ||
        httpd_req_get_hdr_value_str(req,"Content-Type",value,sizeof(value))!=ESP_OK ||
        strcmp(value,"application/json")) {
        reply(req,"403 Forbidden",API_ERROR("INVALID_MANAGE_MARKER", "管理请求标识无效")); return false;
    }
    size_t n=httpd_req_get_hdr_value_len(req,"Origin");
    if (n && (httpd_req_get_hdr_value_str(req,"Origin",value,sizeof(value))!=ESP_OK || strcmp(value,origin))) {
        reply(req,"403 Forbidden",API_ERROR("CROSS_ORIGIN_REJECTED", "跨来源操作被拒绝")); return false;
    }
    if (rebooting) { reply(req,"409 Conflict",API_ERROR("RESTART_PENDING", "即将重启，请稍后重连")); return false; }
    return true;
}
static bool authorize(httpd_req_t *req,bool mutation)
{
    if (gateway_settings_needs_setup()) {
        reply(req,"423 Locked",API_ERROR("SETUP_REQUIRED", "请先打开管理首页设置口令")); return false;
    }
    char supplied[sizeof(authorization)]={0};
    size_t len=httpd_req_get_hdr_value_len(req,"Authorization");
    bool ok=len==strlen(authorization) && len<sizeof(supplied) &&
        httpd_req_get_hdr_value_str(req,"Authorization",supplied,sizeof(supplied))==ESP_OK;
    unsigned diff=0;
    /* Compare all bytes after checking the public length. */
    if (ok) for (size_t i=0;i<len;i++) diff|=(unsigned char)supplied[i]^(unsigned char)authorization[i];
    if (!ok || diff) {
        httpd_resp_set_hdr(req,"WWW-Authenticate","Basic realm=\"ESP32 Management\", charset=\"UTF-8\"");
        reply(req,"401 Unauthorized",API_ERROR("AUTH_REQUIRED", "需要管理密码")); return false;
    }
    return !mutation || mutation_allowed(req);
}
static esp_err_t update_authorization(void)
{
    if (gateway_settings_needs_setup()) { authorization[0]=0; return ESP_OK; }
    char credentials[72]; snprintf(credentials,sizeof(credentials),"admin:%s",gateway_settings()->admin_password);
    size_t encoded; strcpy(authorization,"Basic ");
    int err=mbedtls_base64_encode((unsigned char *)authorization+6,sizeof(authorization)-7,&encoded,
        (unsigned char *)credentials,strlen(credentials));
    memset(credentials,0,sizeof(credentials));
    if (err) return ESP_FAIL;
    authorization[encoded+6]=0; return ESP_OK;
}
static esp_err_t ap_session(httpd_handle_t handle,int fd)
{
    struct sockaddr_in local,peer; socklen_t len=sizeof(local);
    if (getsockname(fd,(struct sockaddr *)&local,&len) || local.sin_family!=AF_INET ||
        local.sin_addr.s_addr!=ap_address.ip.addr) return ESP_FAIL;
    len=sizeof(peer);
    if (getpeername(fd,(struct sockaddr *)&peer,&len) || peer.sin_family!=AF_INET ||
        (peer.sin_addr.s_addr&ap_address.netmask.addr)!=(ap_address.ip.addr&ap_address.netmask.addr)) return ESP_FAIL;
    return management_http_open(handle,fd);
}
static esp_err_t send_json(httpd_req_t *req,cJSON *root)
{
    if (!root) return reply(req,"503 Service Unavailable",API_ERROR("OUT_OF_MEMORY", "内存不足"));
    char *body=cJSON_PrintUnformatted(root); cJSON_Delete(root);
    if (!body) return reply(req,"503 Service Unavailable",API_ERROR("OUT_OF_MEMORY", "内存不足"));
    esp_err_t err=reply(req,"200 OK",body); cJSON_free(body); return err;
}
static esp_err_t page(httpd_req_t *req)
{
    bool setup=gateway_settings_needs_setup();
    if (!setup && !authorize(req,false)) return ESP_OK;
    httpd_resp_set_type(req,"text/html; charset=utf-8");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"X-Content-Type-Options","nosniff");
    httpd_resp_set_hdr(req,"Content-Security-Policy","default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'");
    return setup?httpd_resp_send(req,(const char *)setup_start,setup_end-setup_start-1):
        httpd_resp_send(req,(const char *)page_start,page_end-page_start-1);
}
static void ip_text(uint32_t ip,char out[16])
{
    struct in_addr address={.s_addr=ip};
    if (!inet_ntop(AF_INET,&address,out,16)) strcpy(out,"0.0.0.0");
}
static cJSON *body(httpd_req_t *req);
static gateway_event_t event_scratch[32]; /* HTTP owner task only; avoid a large stack copy. */
static esp_err_t events_api(httpd_req_t *req)
{
    if(!authorize(req,false))return ESP_OK;
    cJSON *root=cJSON_CreateObject();if(!root)return send_json(req,NULL);
    cJSON *array=cJSON_AddArrayToObject(root,"events");
    if(!array){cJSON_Delete(root);return send_json(req,NULL);}
    size_t n=gateway_events_snapshot(event_scratch,32);
    cJSON_AddNumberToObject(root,"capacity",GATEWAY_EVENT_CAPACITY);
    cJSON_AddNumberToObject(root,"shown",n);
    for(size_t i=0;i<n;i++) {
        const gateway_event_t *e=&event_scratch[i];cJSON *item=cJSON_CreateObject();
        if(!item){cJSON_Delete(root);return send_json(req,NULL);}cJSON_AddItemToArray(array,item);
        cJSON_AddStringToObject(item,"event",gateway_event_name(e->kind));
        cJSON_AddStringToObject(item,"event_code",gateway_event_code(e->kind));
        cJSON_AddNumberToObject(item,"sequence",e->sequence);
        cJSON_AddNumberToObject(item,"first_ms",(double)e->first_ms);
        cJSON_AddNumberToObject(item,"last_ms",(double)e->last_ms);
        cJSON_AddNumberToObject(item,"count",e->count);cJSON_AddNumberToObject(item,"detail",e->detail);
    }
    return send_json(req,root);
}
static void add_runtime_status(cJSON *root)
{
    gateway_state_t state;gateway_state_snapshot(&state);
    cJSON_AddBoolToObject(root,"forwarding_ready",state.ap_up && state.sta_ip && !state.subnet_conflict);
    cJSON_AddNumberToObject(root,"sta_disconnect_reason",state.sta_reason);
#define STATE_NUMBER(field) cJSON_AddNumberToObject(root,#field,(double)state.field)
    STATE_NUMBER(tcp_connect_ms);STATE_NUMBER(tcp_data_ms);STATE_NUMBER(tcp_error_ms);
    STATE_NUMBER(udp_reply_ms);STATE_NUMBER(dns_reply_ms);STATE_NUMBER(udp_error_ms);
    STATE_NUMBER(tcp_errors);STATE_NUMBER(udp_errors);STATE_NUMBER(tcp_error);STATE_NUMBER(udp_error);
    STATE_NUMBER(internet_checked_ms);STATE_NUMBER(internet_success_ms);STATE_NUMBER(internet_failures);
#undef STATE_NUMBER
    cJSON_AddNumberToObject(root,"internet_state",gateway_internet_status(&state,gateway_state_clock_ms()));
    gateway_control_snapshot_t control;gateway_control_snapshot(&control);
    cJSON_AddBoolToObject(root,"control_available",control.available);
    cJSON_AddBoolToObject(root,"job_running",control.running);
    cJSON_AddNumberToObject(root,"job_id",control.job);cJSON_AddNumberToObject(root,"job_action",control.action);
    cJSON_AddNumberToObject(root,"job_result",control.result);cJSON_AddNumberToObject(root,"control_stack_min",control.stack_min);
    cJSON_AddNumberToObject(root,"control_stack_total",6144);
    cJSON_AddBoolToObject(root,"led_enabled",control.led_enabled);cJSON_AddBoolToObject(root,"led_ready",control.led_ready);
    cJSON_AddNumberToObject(root,"led_color",control.led_color);cJSON_AddNumberToObject(root,"led_error",control.led_error);
    cJSON_AddBoolToObject(root,"internet_auto",control.internet_auto);cJSON_AddBoolToObject(root,"job_auto",control.job_auto);
    cJSON_AddNumberToObject(root,"internet_target",control.internet_target);
    cJSON_AddStringToObject(root,"internet_url",gateway_internet_target_url(control.internet_target));
    cJSON_AddNumberToObject(root,"internet_next_ms",(double)control.next_check_ms);
    cJSON_AddNumberToObject(root,"internet_valid_seconds",GW_INTERNET_VALID_MS/1000);
    cJSON_AddNumberToObject(root,"internet_http_status",control.internet_probe.http_status);
    char resolved[16];ip_text(control.internet_probe.ipv4,resolved);cJSON_AddStringToObject(root,"internet_ipv4",resolved);
    cJSON *internet=cJSON_AddArrayToObject(root,"internet_probe");
    if(internet)for(unsigned i=0;i<4;i++) {
        cJSON *step=cJSON_CreateObject();if(!step)break;cJSON_AddItemToArray(internet,step);
        cJSON_AddNumberToObject(step,"code",i<3?control.internet_probe.network.code[i]:control.internet_probe.http_code);
        cJSON_AddNumberToObject(step,"ms",i<3?control.internet_probe.network.elapsed_ms[i]:control.internet_probe.http_ms);
    }
    cJSON *array=cJSON_AddArrayToObject(root,"probe");
    if(array)for(unsigned i=0;i<3;i++) {
        cJSON *step=cJSON_CreateObject();if(!step)break;cJSON_AddItemToArray(array,step);
        cJSON_AddNumberToObject(step,"code",control.probe.code[i]);cJSON_AddNumberToObject(step,"ms",control.probe.elapsed_ms[i]);
    }
}
static esp_err_t action_api(httpd_req_t *req)
{
    if(!authorize(req,true))return ESP_OK;
    cJSON *root=body(req);
    const cJSON *action=cJSON_GetObjectItemCaseSensitive(root,"action");
    if(!cJSON_IsString(action)) {cJSON_Delete(root);return reply(req,"400 Bad Request",API_ERROR("ACTION_MISSING", "缺少维护操作"));}
    esp_err_t err;
    if(!strcmp(action->valuestring,"internet_settings")) {
        const cJSON *enabled=cJSON_GetObjectItemCaseSensitive(root,"enabled"),*target=cJSON_GetObjectItemCaseSensitive(root,"target");
        if(cJSON_GetArraySize(root)!=3 || !cJSON_IsBool(enabled) || !cJSON_IsNumber(target) ||
            target->valuedouble<0 || target->valuedouble>=GATEWAY_INTERNET_TARGETS || target->valuedouble!=(int)target->valuedouble) {
            cJSON_Delete(root);return reply(req,"400 Bad Request",API_ERROR("INVALID_INTERNET_SETTINGS", "检查开关或目标无效"));
        }
        err=gateway_control_set_internet(cJSON_IsTrue(enabled),(unsigned)target->valueint);cJSON_Delete(root);
        return reply(req,err==ESP_OK?"200 OK":"409 Conflict",err==ESP_OK?"{\"saved\":true}":API_ERROR("INTERNET_SETTINGS_FAILED", "维护执行中或检查设置保存失败，请稍后重试"));
    }
    if(!strcmp(action->valuestring,"led")) {
        const cJSON *enabled=cJSON_GetObjectItemCaseSensitive(root,"enabled");
        if(cJSON_GetArraySize(root)!=2 || !cJSON_IsBool(enabled)) {cJSON_Delete(root);return reply(req,"400 Bad Request",API_ERROR("INVALID_LED_SETTING", "灯开关需为布尔值"));}
        err=gateway_control_set_led(cJSON_IsTrue(enabled));cJSON_Delete(root);
        return reply(req,err==ESP_OK?"200 OK":"503 Service Unavailable",err==ESP_OK?"{\"saved\":true}":API_ERROR("LED_SETTINGS_FAILED", "状态灯设置保存失败"));
    }
    gateway_action_t kind=!strcmp(action->valuestring,"reconnect")?GW_ACTION_RECONNECT:
        !strcmp(action->valuestring,"relay_reset")?GW_ACTION_RELAY_RESET:
        !strcmp(action->valuestring,"reboot")?GW_ACTION_REBOOT:
        !strcmp(action->valuestring,"diagnose")?GW_ACTION_DIAGNOSE:
        !strcmp(action->valuestring,"internet")?GW_ACTION_INTERNET:GW_ACTION_NONE;
    bool valid=cJSON_GetArraySize(root)==1 && kind!=GW_ACTION_NONE;cJSON_Delete(root);
    if(!valid)return reply(req,"400 Bad Request",API_ERROR("INVALID_ACTION", "维护操作无效"));
    if(kind==GW_ACTION_REBOOT && gateway_settings_pending())return reply(req,"409 Conflict",API_ERROR("CONFIRM_BEFORE_RESTART", "请先确认新配置，未确认重启会回滚"));
    err=gateway_control_submit(kind);
    if(err!=ESP_OK)return reply(req,"409 Conflict",API_ERROR("CONTROL_UNAVAILABLE", "维护任务正在执行或不可用，请稍后重试"));
    gateway_control_snapshot_t control;gateway_control_snapshot(&control);
    char response[96];snprintf(response,sizeof(response),"{\"queued\":true,\"job_id\":%lu,\"restart\":%s}",
        (unsigned long)control.job,kind==GW_ACTION_REBOOT?"true":"false");
    return reply(req,"200 OK",response);
}
static esp_err_t status(httpd_req_t *req)
{
    if (!authorize(req,false)) return ESP_OK;
    cJSON *root=cJSON_CreateObject(); if (!root) return send_json(req,NULL);
    esp_netif_ip_info_t sta={0}; esp_netif_get_ip_info(sta_interface,&sta);
    wifi_ap_record_t upstream={0}; bool connected=esp_wifi_sta_get_ap_info(&upstream)==ESP_OK;
    wifi_sta_list_t stations={0}; esp_wifi_ap_get_sta_list(&stations);
    char ip[16]; ip_text(sta.ip.addr,ip);
    cJSON_AddStringToObject(root,"firmware",esp_app_get_description()->version);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    cJSON_AddBoolToObject(root,"verbose_diagnostics",true);
#else
    cJSON_AddBoolToObject(root,"verbose_diagnostics",false);
#endif
#if CONFIG_PROXY_LINK_PROBE
    cJSON_AddBoolToObject(root,"automatic_link_probe",true);
#else
    cJSON_AddBoolToObject(root,"automatic_link_probe",false);
#endif
    cJSON_AddNumberToObject(root,"boot_id",boot_id);
    add_runtime_status(root);
    cJSON_AddStringToObject(root,"sta_ip",ip);
    ip_text(sta.netmask.addr,ip); cJSON_AddStringToObject(root,"sta_netmask",ip);
    ip_text(ap_address.ip.addr,ip); cJSON_AddStringToObject(root,"ap_ip",ip);
    ip_text(ap_address.netmask.addr,ip); cJSON_AddStringToObject(root,"ap_netmask",ip);
    /* Compare both prefixes to detect overlap even when their masks differ. */
    bool overlap=connected && gateway_settings_ap_overlap(gateway_settings(),
        ntohl(sta.ip.addr),ntohl(sta.netmask.addr));
    cJSON_AddBoolToObject(root,"subnet_overlap",overlap);
    cJSON_AddNumberToObject(root,"ap_client_limit",gateway_settings()->max_clients);
    cJSON_AddNumberToObject(root,"tcp_flow_limit",SOCKS_MAX_SESSIONS);
    cJSON_AddNumberToObject(root,"udp_flow_limit",PROXY_MAX_UDP_FLOWS);
    cJSON_AddNumberToObject(root,"tcp_pcb_limit",MEMP_NUM_TCP_PCB);
    cJSON_AddNumberToObject(root,"http_stack_total",HTTP_STACK_BYTES);
    cJSON_AddBoolToObject(root,"sta_connected",connected && sta.ip.addr);
    cJSON_AddNumberToObject(root,"sta_rssi",connected?upstream.rssi:0);
    cJSON_AddNumberToObject(root,"channel",connected?upstream.primary:0);
    cJSON_AddNumberToObject(root,"uptime_seconds",esp_timer_get_time()/1000000);
    cJSON_AddNumberToObject(root,"reset_reason",esp_reset_reason());
    cJSON_AddNumberToObject(root,"internal_total",heap_caps_get_total_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root,"psram_total",heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root,"internal_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root,"internal_min",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root,"internal_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
    cJSON_AddNumberToObject(root,"psram_free",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root,"http_stack_min",uxTaskGetStackHighWaterMark(NULL));
    cJSON_AddBoolToObject(root,"pending",gateway_settings_pending());
    int64_t remaining=(confirmation_deadline-esp_timer_get_time())/1000000;
    cJSON_AddNumberToObject(root,"confirm_seconds",gateway_settings_pending() && remaining>0?remaining:0);
    net_diagnostics_snapshot_t metrics; net_diagnostics_snapshot(&metrics);
#define METRIC(name) cJSON_AddNumberToObject(root,#name,metrics.name)
    METRIC(tcp_pcb_live); METRIC(tcp_pcb_peak); METRIC(tcp_pcb_failures);
    METRIC(ap_tx_errors); METRIC(sta_tx_errors); METRIC(clone_failures);
    METRIC(client_rto); METRIC(upstream_rto); METRIC(client_fast); METRIC(upstream_fast);
#undef METRIC
    proxy_client_info_t clients[PROXY_AP_MAX_CLIENTS]; proxy_clients_snapshot(clients);
    cJSON *array=cJSON_AddArrayToObject(root,"clients");
    if (!array) { cJSON_Delete(root); return send_json(req,NULL); }
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (clients[i].connected) {
        proxy_client_info_t *c=&clients[i];
        cJSON *item=cJSON_CreateObject();
        if (!item) { cJSON_Delete(root); return send_json(req,NULL); }
        cJSON_AddItemToArray(array,item);
        char mac[18]; snprintf(mac,sizeof(mac),MACSTR,MAC2STR(c->mac)); ip_text(c->ip,ip);
        cJSON_AddStringToObject(item,"mac",mac); cJSON_AddStringToObject(item,"ip",ip);
        cJSON_AddNumberToObject(item,"token",c->token);
        cJSON_AddNumberToObject(item,"tcp",c->tcp); cJSON_AddNumberToObject(item,"udp",c->udp);
        cJSON_AddNumberToObject(item,"tcp_denied",c->tcp_denied); cJSON_AddNumberToObject(item,"udp_denied",c->udp_denied);
        cJSON_AddNumberToObject(item,"up_bytes",(double)c->tcp_up+c->udp_up);
        cJSON_AddNumberToObject(item,"down_bytes",(double)c->tcp_down+c->udp_down);
        for (unsigned j=0;j<stations.num;j++) if (!memcmp(c->mac,stations.sta[j].mac,6))
            cJSON_AddNumberToObject(item,"rssi",stations.sta[j].rssi);
    }
    esp_err_t err=send_json(req,root);
    if (err==ESP_OK) {
        struct sockaddr_in peer; socklen_t size=sizeof(peer);
        if (!getpeername(httpd_req_to_sockfd(req),(struct sockaddr *)&peer,&size)) {
            bool seen=false;
            for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (status_peers[i]==peer.sin_addr.s_addr) seen=true;
            if (!seen) for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (!status_peers[i]) {
                status_peers[i]=peer.sin_addr.s_addr;
                char peer_ip[16]; ip_text(peer.sin_addr.s_addr,peer_ip);
                ESP_LOGI("management","First status API response sent peer=%s stack_free=%u",
                         peer_ip,(unsigned)uxTaskGetStackHighWaterMark(NULL));
                break;
            }
        }
    }
    return err;
}
static esp_err_t config(httpd_req_t *req)
{
    if (!authorize(req,false)) return ESP_OK;
    const gateway_settings_t *s=gateway_settings();
    cJSON *root=cJSON_CreateObject(); if (!root) return send_json(req,NULL);
    cJSON_AddStringToObject(root,"sta_ssid",s->sta_ssid);
    cJSON_AddBoolToObject(root,"sta_password_set",s->sta_password[0]!=0);
    cJSON_AddStringToObject(root,"ap_ssid",s->ap_ssid);
    cJSON_AddStringToObject(root,"socks_ipv4",s->socks_ipv4);
    cJSON_AddStringToObject(root,"dns_ipv4",s->dns_ipv4);
    cJSON_AddNumberToObject(root,"socks_port",s->socks_port);
    cJSON_AddNumberToObject(root,"max_clients",s->max_clients);
    cJSON_AddNumberToObject(root,"ap_subnet",s->ap_subnet);
    return send_json(req,root);
}
/* Bound parser nesting before cJSON recursion, prohibit embedded NUL and duplicate keys. */
static cJSON *body(httpd_req_t *req)
{
    if (!req->content_len || req->content_len>BODY_MAX) return NULL;
    char buffer[BODY_MAX+1]; size_t used=0;
    int64_t deadline=esp_timer_get_time()+3000000;
    while (used<req->content_len) {
        if (esp_timer_get_time()>=deadline) return NULL;
        int n=management_http_recv(req,buffer+used,req->content_len-used);
        if (n<=0) return NULL;
        used+=n;
    }
    buffer[used]=0;
    return management_json_parse(buffer,used);
}
static esp_err_t first_setup(httpd_req_t *req)
{
    if (!gateway_settings_needs_setup())
        return reply(req,"409 Conflict",API_ERROR("SETUP_LOCKED", "口令已经设置，请登录管理页"));
    if (!mutation_allowed(req)) return ESP_OK;
    cJSON *root=body(req);
    const cJSON *password=cJSON_GetObjectItemCaseSensitive(root,"password");
    const cJSON *confirmation=cJSON_GetObjectItemCaseSensitive(root,"confirm");
    bool valid=cJSON_IsString(password) && cJSON_IsString(confirmation) &&
        cJSON_GetArraySize(root)==2 && !strcmp(password->valuestring,confirmation->valuestring);
    esp_err_t err=valid?gateway_settings_setup_password(password->valuestring):ESP_ERR_INVALID_ARG;
    cJSON_Delete(root);
    if (err!=ESP_OK) return reply(req,err==ESP_ERR_INVALID_STATE?"409 Conflict":"400 Bad Request",
        API_ERROR("INVALID_SETUP", "两次口令需一致且为 12～64 字节，或配置当前不可保存"));
    if (update_authorization()!=ESP_OK)
        return reply(req,"500 Internal Server Error",API_ERROR("SETUP_RESTART_REQUIRED", "口令已保存，请重启后登录"));
    ESP_LOGI("management","Initial management password set; setup endpoint is now locked");
    return reply(req,"200 OK","{\"configured\":true}");
}
static void timer_work(void *arg)
{
    (void)arg;
    if (rebooting) { esp_restart(); return; }
    if (gateway_settings_pending() && esp_timer_get_time()>=confirmation_deadline) {
        esp_err_t err=gateway_settings_rollback();
        ESP_LOGW("management","Confirmation timeout; rollback=%s",esp_err_to_name(err));
        /* If NVS write fails, persisted trial=2 still restores on the next boot. */
        esp_restart();
    }
}
static void timer_tick(void *arg)
{
    (void)arg;
    /* Execute mutations on the HTTP owner task. Retry next tick if its queue is full. */
    if (httpd_queue_work(server,timer_work,NULL)!=ESP_OK)
        ESP_LOGW("management","Management timer queue busy");
}
static esp_err_t staged(httpd_req_t *req,const gateway_settings_t *candidate)
{
    gateway_control_snapshot_t control;gateway_control_snapshot(&control);
    if(control.running)return reply(req,"409 Conflict",API_ERROR("CONTROL_BUSY", "维护任务执行中，请结束后再保存配置"));
    esp_netif_ip_info_t sta={0};
    esp_netif_get_ip_info(sta_interface,&sta);
    /* A changed STA SSID has an unknown future subnet; recheck on GOT_IP. */
    if (!strcmp(candidate->sta_ssid,gateway_settings()->sta_ssid) &&
        gateway_settings_ap_overlap(candidate,ntohl(sta.ip.addr),ntohl(sta.netmask.addr)))
        return reply(req,"400 Bad Request",API_ERROR("SUBNET_OVERLAP", "AP 网段与当前上游 Wi-Fi 重叠，请换一个第三段数字"));
    esp_err_t err=gateway_settings_stage(candidate);
    if (err!=ESP_OK) return reply(req,err==ESP_ERR_INVALID_STATE?"409 Conflict":"400 Bad Request",
        API_ERROR("CONFIG_STAGE_FAILED", "配置无效、尚未确认上一份配置或保存失败"));
    rebooting=true;
    /* Timer runs at 1 s; response is sent before the next scheduled restart. */
    esp_timer_stop(timer); esp_timer_start_periodic(timer,2000000);
    char next_ip[16],response[144]; gateway_settings_ap_ip(candidate,next_ip);
    snprintf(response,sizeof(response),"{\"reboot\":true,\"confirm_seconds\":180,\"management_url\":\"http://%s/\"}",next_ip);
    return reply(req,"200 OK",response);
}
static esp_err_t save(httpd_req_t *req)
{
    if (!authorize(req,true)) return ESP_OK;
    cJSON *root=body(req); if (!root) return reply(req,"400 Bad Request",API_ERROR("INVALID_JSON", "JSON 无效或超过 1024 字节"));
    gateway_settings_t next=*gateway_settings();
    bool valid=management_settings_patch(root,&next);
    cJSON_Delete(root);
    if (!valid) return reply(req,"400 Bad Request",API_ERROR("INVALID_FIELDS", "字段、类型或长度无效"));
    return staged(req,&next);
}
static esp_err_t confirm(httpd_req_t *req)
{
    if (!authorize(req,true)) return ESP_OK;
    cJSON *root=body(req); bool valid=root && !root->child; cJSON_Delete(root);
    if (!valid) return reply(req,"400 Bad Request",API_ERROR("EMPTY_OBJECT_REQUIRED", "请求必须为 {}"));
    if (gateway_settings_pending() && esp_timer_get_time()>=confirmation_deadline) {
        timer_work(NULL); return ESP_FAIL;
    }
    esp_err_t err=gateway_settings_confirm();
    if (err==ESP_OK) esp_timer_stop(timer);
    return reply(req,err==ESP_OK?"200 OK":"409 Conflict",err==ESP_OK?
        "{\"confirmed\":true}":API_ERROR("CONFIRM_FAILED", "没有待确认配置或保存失败"));
}
static esp_err_t defaults(httpd_req_t *req)
{
    if (!authorize(req,true)) return ESP_OK;
    cJSON *root=body(req); bool valid=root && !root->child; cJSON_Delete(root);
    if (!valid) return reply(req,"400 Bad Request",API_ERROR("EMPTY_OBJECT_REQUIRED", "请求必须为 {}"));
    gateway_settings_t next; gateway_settings_defaults(&next);
    strcpy(next.admin_password,gateway_settings()->admin_password);
    return staged(req,&next);
}
static esp_err_t kick(httpd_req_t *req)
{
    if (!authorize(req,true)) return ESP_OK;
    cJSON *root=body(req); const cJSON *v=cJSON_GetObjectItemCaseSensitive(root,"token");
    bool valid=root && root->child==v && v && !v->next && management_number(v,1,UINT32_MAX);
    uint32_t token=valid?(uint32_t)v->valuedouble:0; cJSON_Delete(root);
    if (!valid) return reply(req,"400 Bad Request",API_ERROR("INVALID_CLIENT_TOKEN", "客户端标识无效"));
    proxy_client_info_t clients[PROXY_AP_MAX_CLIENTS]; proxy_clients_snapshot(clients);
    for (unsigned i=0;i<PROXY_AP_MAX_CLIENTS;i++) if (clients[i].connected && clients[i].token==token) {
        uint16_t aid;
        if (!proxy_client_current(token) || esp_wifi_ap_get_sta_aid(clients[i].mac,&aid)!=ESP_OK || !aid || !proxy_client_current(token))
            break;
        esp_err_t err=esp_wifi_deauth_sta(aid);
        return reply(req,err==ESP_OK?"200 OK":"409 Conflict",err==ESP_OK?
            "{\"disconnected\":true}":API_ERROR("CLIENT_DISCONNECT_FAILED", "断开失败，请刷新"));
    }
    return reply(req,"409 Conflict",API_ERROR("CLIENT_CHANGED", "客户端已经离线或重连，请刷新"));
}
static esp_err_t managed_request(httpd_req_t *req)
{
    const httpd_uri_t *route=req->user_ctx;
    management_http_begin(req);
    esp_err_t result=req->content_len>BODY_MAX?
        reply(req,"413 Payload Too Large",API_ERROR("INVALID_JSON", "JSON 无效或超过 1024 字节")):
        route->handler(req);
    return management_http_finish(req,result);
}
static esp_err_t missing_route(httpd_req_t *req,httpd_err_code_t error)
{
    /* Unregistered routes also close rather than retain a receive deadline. */
    httpd_resp_send_err(req,error,NULL);
    return ESP_FAIL;
}
esp_err_t web_management_start(esp_netif_t *ap,esp_netif_t *sta)
{
    boot_id=esp_random();
    sta_interface=sta;
    esp_err_t err=esp_netif_get_ip_info(ap,&ap_address); if (err!=ESP_OK) return err;
    err=esp_netif_get_netif_impl_name(ap,ap_interface.ifr_name); if (err!=ESP_OK) return err;
    char ip[16]; ip_text(ap_address.ip.addr,ip); snprintf(origin,sizeof(origin),"http://%s",ip);
    err=update_authorization(); if (err!=ESP_OK) return err;
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.server_port=80; cfg.stack_size=HTTP_STACK_BYTES; cfg.task_priority=3;
    cfg.max_open_sockets=2; cfg.lru_purge_enable=true;
    cfg.recv_wait_timeout=cfg.send_wait_timeout=3;
    cfg.max_req_hdr_len=1024; cfg.max_uri_handlers=10;
    cfg.if_name=&ap_interface; cfg.open_fn=ap_session;cfg.close_fn=management_http_close;
    size_t before=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    err=httpd_start(&server,&cfg); if (err!=ESP_OK) return err;
    static const httpd_uri_t routes[]={
        {.uri="/",.method=HTTP_GET,.handler=page},
        {.uri="/api/setup",.method=HTTP_POST,.handler=first_setup},
        {.uri="/api/events",.method=HTTP_GET,.handler=events_api},
        {.uri="/api/action",.method=HTTP_POST,.handler=action_api},
        {.uri="/api/status",.method=HTTP_GET,.handler=status},
        {.uri="/api/config",.method=HTTP_GET,.handler=config},
        {.uri="/api/config",.method=HTTP_POST,.handler=save},
        {.uri="/api/confirm",.method=HTTP_POST,.handler=confirm},
        {.uri="/api/defaults",.method=HTTP_POST,.handler=defaults},
        {.uri="/api/kick",.method=HTTP_POST,.handler=kick},
    };
    for (unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);i++) {
        httpd_uri_t route=routes[i];route.handler=managed_request;route.user_ctx=(void *)&routes[i];
        err=httpd_register_uri_handler(server,&route); if (err!=ESP_OK) break;
    }
    if(err==ESP_OK)err=httpd_register_err_handler(server,HTTPD_404_NOT_FOUND,missing_route);
    if(err==ESP_OK)err=httpd_register_err_handler(server,HTTPD_405_METHOD_NOT_ALLOWED,missing_route);
    if (err==ESP_OK) {
        const esp_timer_create_args_t args={.callback=timer_tick,.name="web_control"};
        err=esp_timer_create(&args,&timer);
    }
    if (err==ESP_OK && gateway_settings_pending()) {
        confirmation_deadline=esp_timer_get_time()+CONFIRM_SECONDS*1000000LL;
        err=esp_timer_start_periodic(timer,1000000);
    }
    if (err!=ESP_OK) { if (timer) esp_timer_delete(timer); httpd_stop(server); server=NULL; return err; }
    size_t after=heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    ESP_LOGI("management","AP-only %s/; max HTTP clients=2 stack=6144 priority=3 internal_delta=%d pending=%d",
        origin,(int)before-(int)after,gateway_settings_pending());
    return ESP_OK;
}
