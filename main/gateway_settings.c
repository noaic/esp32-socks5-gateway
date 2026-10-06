#include "gateway_settings.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "gateway_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* One committed NVS blob: never expose a half-written active/backup pair. */
typedef struct {
    uint32_t version;
    bool bootstrap, previous_bootstrap;
    uint8_t trial; /* 0 confirmed; 1 staged; 2 attempted, rollback on next boot */
    gateway_settings_t active, previous;
} settings_record_t;
/* v1 ended with one padding byte; v2 uses it without moving any existing field. */
_Static_assert(sizeof(gateway_settings_t)==296 && offsetof(gateway_settings_t,ap_subnet)==295,
               "v1 NVS layout migration must be revised if layout changes");
_Static_assert(sizeof(settings_record_t)==600,"NVS record size must preserve v1");
static settings_record_t record;
static gateway_settings_t boot_settings;
static atomic_bool pending,setup_required;
static bool recovery_committed;
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t settings_lock;
void gateway_settings_defaults(gateway_settings_t *s)
{
    memset(s,0,sizeof(*s));
    snprintf(s->sta_ssid,sizeof(s->sta_ssid),"%s",CONFIG_PROXY_STA_SSID);
    snprintf(s->sta_password,sizeof(s->sta_password),"%s",CONFIG_PROXY_STA_PASSWORD);
    snprintf(s->ap_ssid,sizeof(s->ap_ssid),"%s",CONFIG_PROXY_AP_SSID);
    snprintf(s->ap_password,sizeof(s->ap_password),"%s",CONFIG_PROXY_AP_PASSWORD);
    strcpy(s->socks_ipv4,"192.168.1.2"); s->socks_port=7897;
    strcpy(s->dns_ipv4,"1.1.1.1"); s->max_clients=2; s->ap_subnet=4;
}
static esp_err_t store(const settings_record_t *r)
{
    nvs_handle_t h;
    esp_err_t err=nvs_open("gateway",NVS_READWRITE,&h);
    if (err!=ESP_OK) return err;
    err=nvs_set_blob(h,"settings",r,sizeof(*r));
    if (err==ESP_OK) err=nvs_commit(h);
    nvs_close(h); return err;
}
/* Unclaimed records may have an empty password; legacy generated credentials are
 * also accepted only as unclaimed and are never exposed or usable for login. */
static bool stored_settings_valid(const gateway_settings_t *s,bool unclaimed)
{
    if (!unclaimed || s->admin_password[0]) return gateway_settings_valid(s);
    gateway_settings_t candidate=*s;
    strcpy(candidate.admin_password,"not-configured");
    return gateway_settings_valid(&candidate);
}
static esp_err_t init_locked(void)
{
    pending=false;recovery_committed=false;
    nvs_handle_t h;
    esp_err_t err=nvs_open("gateway",NVS_READWRITE,&h);
    if (err!=ESP_OK) return err;
    size_t bytes=sizeof(record);
    err=nvs_get_blob(h,"settings",&record,&bytes); nvs_close(h);
    bool migrated=err==ESP_OK && bytes==sizeof(record) && record.version==1;
    if (migrated) {
        record.active.ap_subnet=record.previous.ap_subnet=4;
        record.version=2;
    }
    bool valid=err==ESP_OK && bytes==sizeof(record) && record.version==2 &&
        record.trial<=2 && stored_settings_valid(&record.active,record.bootstrap) &&
        stored_settings_valid(&record.previous,record.previous_bootstrap);
    if (!valid) {
        if (err!=ESP_OK && err!=ESP_ERR_NVS_NOT_FOUND && err!=ESP_ERR_NVS_INVALID_LENGTH) return err;
        memset(&record,0,sizeof(record)); record.version=2; record.bootstrap=record.previous_bootstrap=true;
        gateway_settings_defaults(&record.active);
        if (!stored_settings_valid(&record.active,true)) return ESP_ERR_INVALID_ARG;
        record.previous=record.active;
        err=store(&record); if (err!=ESP_OK) return err;

    } else {
        if (migrated) {
            err=store(&record); if (err!=ESP_OK) return err;
            ESP_LOGI("management","Settings v1 migrated to v2; AP subnet remains 192.168.4.0/24");
        }
    }
    if (record.trial==2) {
        record.active=record.previous; record.bootstrap=record.previous_bootstrap; record.trial=0;
        err=store(&record); if (err!=ESP_OK) return err;
        gateway_event(GW_CONFIG_ROLLBACK,1);
        ESP_LOGW("management","Unconfirmed configuration restored on reboot");
    } else if (record.trial==1) {
        record.trial=2;
        err=store(&record); if (err!=ESP_OK) return err;
        pending=true;
    }
    if (record.bootstrap)
        ESP_LOGI("management","First visit to AP Web page must set a management password");
    boot_settings=record.active;setup_required=record.bootstrap;
    return ESP_OK;
}
const gateway_settings_t *gateway_settings(void) { return &boot_settings; }
bool gateway_settings_pending(void) { return pending; }
/* HTTP and serial recovery share one mutex; no Flash operation in a spinlock. */
static esp_err_t stage_locked(const gateway_settings_t *s)
{
    if (recovery_committed || record.bootstrap || pending || record.trial) return ESP_ERR_INVALID_STATE;
    if (!gateway_settings_valid(s)) return ESP_ERR_INVALID_ARG;
    settings_record_t next=record;
    next.previous=boot_settings; next.previous_bootstrap=record.bootstrap;
    next.active=*s; next.trial=1;
    if (strcmp(s->admin_password,boot_settings.admin_password)) next.bootstrap=false;
    esp_err_t err=store(&next); if (err==ESP_OK) { record=next;gateway_event(GW_CONFIG_STAGE,0); }
    return err;
}
static esp_err_t confirm_locked(void)
{
    if (recovery_committed || !pending) return ESP_ERR_INVALID_STATE;
    settings_record_t next=record; next.trial=0; next.previous=next.active; next.previous_bootstrap=next.bootstrap;
    esp_err_t err=store(&next);
    if (err==ESP_OK) { record=next; pending=false; gateway_event(GW_CONFIG_CONFIRM,0); }
    return err;
}
static esp_err_t rollback_locked(void)
{
    if (recovery_committed || !pending) return ESP_ERR_INVALID_STATE;
    settings_record_t next=record; next.active=next.previous; next.bootstrap=next.previous_bootstrap; next.trial=0;
    esp_err_t err=store(&next);
    if (err==ESP_OK) { record=next; pending=false; gateway_event(GW_CONFIG_ROLLBACK,0); }
    return err;
}

bool gateway_settings_needs_setup(void) { return setup_required; }
static esp_err_t setup_password_locked(const char *password)
{
    if (recovery_committed || !record.bootstrap || pending || record.trial) return ESP_ERR_INVALID_STATE;
    if (!password || strlen(password)>=sizeof(boot_settings.admin_password)) return ESP_ERR_INVALID_ARG;
    settings_record_t next=record;
    strcpy(next.active.admin_password,password);
    if (!gateway_settings_valid(&next.active)) return ESP_ERR_INVALID_ARG;
    next.previous=next.active;
    next.bootstrap=next.previous_bootstrap=false;
    esp_err_t err=store(&next);
    if (err==ESP_OK) {
        record=next;gateway_event(GW_PASSWORD_SET,0);
        /* Initial claim changes credentials only, not the live network configuration. */
        strcpy(boot_settings.admin_password,password);setup_required=false;
    }
    return err;
}

static esp_err_t recovery_locked(bool network,char web_ip[16])
{
    if(recovery_committed || record.trial==1)return ESP_ERR_INVALID_STATE;
    settings_record_t next={.version=2};
    if(network) {
        gateway_settings_defaults(&next.active);
        memcpy(next.active.admin_password,boot_settings.admin_password,sizeof(next.active.admin_password));
        next.bootstrap=setup_required;
    } else {
        next.active=boot_settings;
        memset(next.active.admin_password,0,sizeof(next.active.admin_password));
        next.bootstrap=true;
    }
    if(!stored_settings_valid(&next.active,next.bootstrap))return ESP_ERR_INVALID_ARG;
    next.previous=next.active;next.previous_bootstrap=next.bootstrap;
    esp_err_t err=store(&next);
    if(err==ESP_OK) {
        /* Keep live credentials/network unchanged until restart; refuse subsequent
         * HTTP writes, and prevent the trial timer from overwriting this commit. */
        if(web_ip)gateway_settings_ap_ip(&next.active,web_ip);
        recovery_committed=true;pending=false;
        gateway_event(GW_RECOVERY_COMMIT,network);
    }
    return err;
}
static bool take_settings_lock(void)
{return settings_lock && xSemaphoreTake(settings_lock,portMAX_DELAY)==pdTRUE;}
esp_err_t gateway_settings_init(void)
{
    /* Called on the startup task before HTTP or control workers exist. */
    if(!settings_lock)settings_lock=xSemaphoreCreateMutexStatic(&lock_storage);
    if(!take_settings_lock())return ESP_ERR_NO_MEM;
    esp_err_t err=init_locked();xSemaphoreGive(settings_lock);return err;
}
esp_err_t gateway_settings_stage(const gateway_settings_t *settings)
{
    if(!take_settings_lock())return ESP_ERR_INVALID_STATE;
    esp_err_t err=stage_locked(settings);xSemaphoreGive(settings_lock);return err;
}
esp_err_t gateway_settings_confirm(void)
{
    if(!take_settings_lock())return ESP_ERR_INVALID_STATE;
    esp_err_t err=confirm_locked();xSemaphoreGive(settings_lock);return err;
}
esp_err_t gateway_settings_rollback(void)
{
    if(!take_settings_lock())return ESP_ERR_INVALID_STATE;
    esp_err_t err=rollback_locked();xSemaphoreGive(settings_lock);return err;
}
esp_err_t gateway_settings_setup_password(const char *password)
{
    if(!take_settings_lock())return ESP_ERR_INVALID_STATE;
    esp_err_t err=setup_password_locked(password);xSemaphoreGive(settings_lock);return err;
}
esp_err_t gateway_settings_recover(bool network,char web_ip[16])
{
    if(!take_settings_lock())return ESP_ERR_INVALID_STATE;
    esp_err_t err=recovery_locked(network,web_ip);xSemaphoreGive(settings_lock);return err;
}
