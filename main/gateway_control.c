#include "gateway_control.h"
#include "serial_console.h"
#include <stdio.h>
#include "gateway_settings.h"
#include "gateway_state.h"
#include "socks_relay.h"
#include "udp_gateway.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_log.h"
#include "led_strip.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
static portMUX_TYPE control_lock=portMUX_INITIALIZER_UNLOCKED;
static gateway_control_snapshot_t control;
static QueueHandle_t jobs;
static led_strip_handle_t strip;
static uint32_t schedule_generation;
static bool preferences_busy;
typedef struct {uint8_t version,enabled,target,reserved;} health_preferences_t;
static gateway_internet_result_t empty_internet(void)
{return (gateway_internet_result_t){.network.code={5000,5000,5000},.http_code=5000};}
static void led_update(void)
{
    if(!strip)return;
    gateway_state_t state;gateway_state_snapshot(&state);
    gateway_control_snapshot_t current;gateway_control_snapshot(&current);
    unsigned color=gateway_state_led_color(&state,current.led_enabled,gateway_settings()->sta_ssid[0]!=0,gateway_state_clock_ms());
    if(color!=current.led_color) {
        static const uint8_t rgb[][3]={{0,0,0},{0,0,8},{8,0,0},{0,8,0},{8,5,0},{8,0,8}};
        esp_err_t err=led_strip_set_pixel(strip,0,rgb[color][0],rgb[color][1],rgb[color][2]);
        if(err==ESP_OK)err=led_strip_refresh(strip);
        portENTER_CRITICAL(&control_lock);control.led_error=err;if(err==ESP_OK)control.led_color=color;portEXIT_CRITICAL(&control_lock);
        if(err!=ESP_OK)gateway_event(GW_LED_ERROR,err);
    }
}
void gateway_control_snapshot(gateway_control_snapshot_t *out)
{portENTER_CRITICAL(&control_lock);*out=control;portEXIT_CRITICAL(&control_lock);}
static void maintenance_idle(void){serial_console_poll();led_update();}
static bool automatic_job(gateway_action_t *action)
{
    gateway_state_t state;gateway_state_snapshot(&state);uint64_t now=gateway_state_clock_ms();
    bool ready=state.ap_up && state.sta_ip && !state.subnet_conflict && !gateway_settings_pending();
    portENTER_CRITICAL(&control_lock);
    if(schedule_generation!=state.network_generation) {
        schedule_generation=state.network_generation;
        control.next_check_ms=ready?now+2000:0;control.internet_probe=empty_internet();
    }
    if(!ready || !control.internet_auto){control.next_check_ms=0;portEXIT_CRITICAL(&control_lock);return false;}
    if(control.running || preferences_busy || now<control.next_check_ms){portEXIT_CRITICAL(&control_lock);return false;}
    control.running=true;control.action=GW_ACTION_INTERNET;control.job++;control.result=0;control.job_auto=true;
    *action=GW_ACTION_INTERNET;portEXIT_CRITICAL(&control_lock);return true;
}
static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        gateway_action_t action;
        bool manual=xQueueReceive(jobs,&action,pdMS_TO_TICKS(200))==pdTRUE;
        if(manual || automatic_job(&action)) {
            gateway_event(GW_MAINT_START,action);
            /* Allow the HTTP response to leave before disconnect/reset/reboot. */
            if(manual)for(unsigned i=0;i<5;i++){maintenance_idle();vTaskDelay(pdMS_TO_TICKS(200));}
            esp_err_t err=ESP_OK;gateway_probe_result_t result={.code={5000,5000,5000}};
            gateway_internet_result_t internet=empty_internet();bool accepted=false;uint32_t check_generation=0;
            switch(action) {
            case GW_ACTION_RECONNECT:
                if(!gateway_settings()->sta_ssid[0])err=ESP_ERR_INVALID_STATE;
                else {
                    err=esp_wifi_disconnect(); /* Existing STA event schedules reconnection. */
                    if(err==ESP_ERR_WIFI_NOT_CONNECT)err=esp_wifi_connect();
                }break;
            case GW_ACTION_RELAY_RESET:
                gateway_internet_invalidate();
#if CONFIG_PROXY_TCP_SOCKS5
                socks_relay_reset_client();
#endif
#if CONFIG_PROXY_UDP_ENABLE
                udp_gateway_reset_client();
#endif
                break;
            case GW_ACTION_REBOOT:
                ESP_LOGI("management","Maintenance reboot requested");esp_restart();break;
            case GW_ACTION_RESET_ADMIN:case GW_ACTION_RESET_NETWORK: {
                bool network=action==GW_ACTION_RESET_NETWORK;
                char ip[16];err=gateway_settings_recover(network,ip);
                if(err==ESP_OK) {
                    printf("OK %s. Rebooting. Web: http://%s/\n",network?"Network restored; admin kept":"Admin cleared; network kept",ip);
                    fflush(stdout);vTaskDelay(pdMS_TO_TICKS(200));esp_restart();
                } else {printf("Reset failed code=%d. Settings unchanged.\n",err);fflush(stdout);}
                break;
            }
            case GW_ACTION_DIAGNOSE: {
                gateway_state_t state;gateway_state_snapshot(&state);
                if(!state.sta_ip || state.subnet_conflict)err=ESP_ERR_INVALID_STATE;
                else gateway_probe_run(gateway_settings()->socks_ipv4,gateway_settings()->socks_port,
                        gateway_settings()->dns_ipv4,&result,maintenance_idle);
                break;
            }
            case GW_ACTION_INTERNET: {
                gateway_state_t state;gateway_state_snapshot(&state);
                if(!state.ap_up || !state.sta_ip || state.subnet_conflict)err=ESP_ERR_INVALID_STATE;
                else {
                    gateway_control_snapshot_t current;gateway_control_snapshot(&current);
                    check_generation=gateway_internet_begin();led_update();
                    gateway_internet_run(gateway_settings()->socks_ipv4,gateway_settings()->socks_port,
                        gateway_settings()->dns_ipv4,current.internet_target,&internet,maintenance_idle);
                    int code=internet.http_code;
                    for(unsigned i=0;i<3;i++)if(internet.network.code[i]){code=internet.network.code[i];break;}
                    accepted=gateway_internet_complete(check_generation,code);
                    err=accepted?code:ESP_ERR_INVALID_STATE;
                    ESP_LOGI("management","Internet check target=%u auto=%d accepted=%d code=%d HTTP=%u",current.internet_target,current.job_auto,accepted,err,internet.http_status);
                }break;
            }
            default: err=ESP_ERR_INVALID_ARG;break;
            }
            int diag_code=err;
            if(!err && action==GW_ACTION_DIAGNOSE)for(unsigned i=0;i<3;i++)if(result.code[i]){diag_code=result.code[i];break;}
            if(action==GW_ACTION_DIAGNOSE)gateway_event(GW_DIAG_DONE,diag_code);
            gateway_event(GW_MAINT_DONE,err);
            uint32_t stack_min=uxTaskGetStackHighWaterMark(NULL);
            portENTER_CRITICAL(&control_lock);
            control.result=err;control.probe=result;control.running=false;
            if(action==GW_ACTION_INTERNET && accepted)control.internet_probe=internet;
            control.stack_min=stack_min;
            portEXIT_CRITICAL(&control_lock);
            if(action==GW_ACTION_INTERNET) {
                gateway_state_t state;gateway_state_snapshot(&state);
                portENTER_CRITICAL(&control_lock);
                if(accepted)schedule_generation=check_generation;
                control.next_check_ms=control.internet_auto?gateway_state_clock_ms()+gateway_internet_retry_ms(state.internet_failures):0;
                portEXIT_CRITICAL(&control_lock);
            }
        }
        maintenance_idle();
    }
}
esp_err_t gateway_control_start(void)
{
    control=(gateway_control_snapshot_t){.led_enabled=true,.led_color=255,.probe={.code={5000,5000,5000}},.internet_auto=true,.internet_probe=empty_internet()};
    schedule_generation=UINT32_MAX;
    preferences_busy=false;
    nvs_handle_t h;esp_err_t err=nvs_open("indicators",NVS_READWRITE,&h);
    if(err==ESP_OK) {
        uint8_t enabled;err=nvs_get_u8(h,"enabled",&enabled);
        if(err==ESP_OK && enabled<=1)control.led_enabled=enabled;
        else if(err!=ESP_ERR_NVS_NOT_FOUND)gateway_event(GW_LED_ERROR,err==ESP_OK?ESP_ERR_INVALID_ARG:err);
        nvs_close(h);
    }else gateway_event(GW_LED_ERROR,err);
    err=nvs_open("healthcfg",NVS_READWRITE,&h);
    if(err==ESP_OK) {
        health_preferences_t pref;size_t size=sizeof(pref);err=nvs_get_blob(h,"prefs",&pref,&size);
        if(err==ESP_OK && size==sizeof(pref) && pref.version==1 && pref.enabled<=1 && pref.target<GATEWAY_INTERNET_TARGETS && !pref.reserved) {
            control.internet_auto=pref.enabled;control.internet_target=pref.target;
        } else if(err!=ESP_ERR_NVS_NOT_FOUND)ESP_LOGW("management","Internet preferences invalid/unavailable: %d",err);
        nvs_close(h);
    }else ESP_LOGW("management","Internet preferences unavailable: %d",err);
    led_strip_config_t config={.strip_gpio_num=48,.max_leds=1,.led_model=LED_MODEL_WS2812,
        .color_component_format=LED_STRIP_COLOR_COMPONENT_FMT_GRB};
    led_strip_rmt_config_t rmt={.resolution_hz=10000000,.flags.with_dma=false};
    err=led_strip_new_rmt_device(&config,&rmt,&strip);
    control.led_ready=err==ESP_OK;control.led_error=err;
    if(err!=ESP_OK)gateway_event(GW_LED_ERROR,err);
    else led_update(); /* Clear immediately if disabled at previous boot. */
    esp_err_t serial_err=serial_console_start();
    if(serial_err!=ESP_OK)ESP_LOGW("management","Serial commands unavailable: %d",serial_err);
    jobs=xQueueCreate(1,sizeof(gateway_action_t));
    if(!jobs)goto failed;
    if(xTaskCreate(worker,"gateway_control",6144,NULL,2,NULL)!=pdPASS)goto failed;
    portENTER_CRITICAL(&control_lock);control.available=true;portEXIT_CRITICAL(&control_lock);
    ESP_LOGI("management","Control worker stack=6144 priority=2; WS2812 GPIO48 ready=%d enabled=%d",control.led_ready,control.led_enabled);
    return ESP_OK;
failed:
    if(jobs){vQueueDelete(jobs);jobs=NULL;}
    if(strip){led_strip_clear(strip);led_strip_del(strip);strip=NULL;}
    control.led_ready=false;control.led_error=ESP_ERR_NO_MEM;
    gateway_event(GW_LED_ERROR,ESP_ERR_NO_MEM);
    return ESP_ERR_NO_MEM;
}
esp_err_t gateway_control_submit(gateway_action_t action)
{
    if(action<GW_ACTION_RECONNECT || action>GW_ACTION_RESET_NETWORK)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&control_lock);
    if(!control.available || control.running || preferences_busy){portEXIT_CRITICAL(&control_lock);return ESP_ERR_INVALID_STATE;}
    control.running=true;control.action=action;control.job++;control.result=0;control.job_auto=false;
    control.probe=(gateway_probe_result_t){.code={5000,5000,5000}};
    portEXIT_CRITICAL(&control_lock);
    if(xQueueSend(jobs,&action,0)!=pdTRUE) {
        portENTER_CRITICAL(&control_lock);control.running=false;portEXIT_CRITICAL(&control_lock);return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t gateway_control_set_led(bool enabled)
{
    gateway_control_snapshot_t current;gateway_control_snapshot(&current);
    if(!current.available)return ESP_ERR_INVALID_STATE;
    if(enabled==current.led_enabled)return ESP_OK;
    nvs_handle_t h;esp_err_t err=nvs_open("indicators",NVS_READWRITE,&h);if(err!=ESP_OK)return err;
    err=nvs_set_u8(h,"enabled",enabled);if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);
    if(err!=ESP_OK)return err;
    portENTER_CRITICAL(&control_lock);control.led_enabled=enabled;portEXIT_CRITICAL(&control_lock);
    gateway_event(GW_LED_SETTING,enabled);return ESP_OK;
}
esp_err_t gateway_control_set_internet(bool enabled,unsigned target)
{
    if(target>=GATEWAY_INTERNET_TARGETS)return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&control_lock);
    if(!control.available || control.running || preferences_busy){portEXIT_CRITICAL(&control_lock);return ESP_ERR_INVALID_STATE;}
    if(control.internet_auto==enabled && control.internet_target==target){portEXIT_CRITICAL(&control_lock);return ESP_OK;}
    preferences_busy=true;portEXIT_CRITICAL(&control_lock);
    health_preferences_t pref={.version=1,.enabled=enabled,.target=(uint8_t)target};
    nvs_handle_t h;esp_err_t err=nvs_open("healthcfg",NVS_READWRITE,&h);
    if(err==ESP_OK){err=nvs_set_blob(h,"prefs",&pref,sizeof(pref));if(err==ESP_OK)err=nvs_commit(h);nvs_close(h);}
    if(err!=ESP_OK){portENTER_CRITICAL(&control_lock);preferences_busy=false;portEXIT_CRITICAL(&control_lock);return err;}
    portENTER_CRITICAL(&control_lock);
    control.internet_auto=enabled;control.internet_target=(uint8_t)target;control.next_check_ms=0;control.internet_probe=empty_internet();
    portEXIT_CRITICAL(&control_lock);
    gateway_internet_invalidate();
    portENTER_CRITICAL(&control_lock);preferences_busy=false;portEXIT_CRITICAL(&control_lock);
    gateway_event(GW_INTERNET_SETTING,(int)target*2+enabled);return ESP_OK;
}
