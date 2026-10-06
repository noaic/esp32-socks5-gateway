"""Exercise actual maintenance queue and persisted LED preferences with SDK adapters."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
SDK=r'''
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_NVS_NOT_FOUND 4
#define ESP_ERR_WIFI_NOT_CONNECT 5
#define CONFIG_PROXY_TCP_SOCKS5 1
#define CONFIG_PROXY_UDP_ENABLE 1
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
typedef unsigned nvs_handle_t;
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef void *QueueHandle_t;
#define pdTRUE 1
#define pdPASS 1
#define pdMS_TO_TICKS(n) (n)
QueueHandle_t xQueueCreate(unsigned,unsigned);
int xQueueSend(QueueHandle_t,const void *,unsigned);
int xQueueReceive(QueueHandle_t,void *,unsigned);
void vQueueDelete(QueueHandle_t);
int xTaskCreate(void (*)(void *),const char *,unsigned,void *,unsigned,void *);
void vTaskDelay(unsigned);
unsigned uxTaskGetStackHighWaterMark(void *);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_connect(void);
void esp_restart(void);
typedef void *led_strip_handle_t;
#define LED_MODEL_WS2812 1
#define LED_STRIP_COLOR_COMPONENT_FMT_GRB 1
typedef struct {int strip_gpio_num;unsigned max_leds;int led_model,color_component_format;} led_strip_config_t;
typedef struct {unsigned resolution_hz;struct {bool with_dma;} flags;} led_strip_rmt_config_t;
esp_err_t led_strip_new_rmt_device(const led_strip_config_t *,const led_strip_rmt_config_t *,led_strip_handle_t *);
esp_err_t led_strip_set_pixel(led_strip_handle_t,unsigned,unsigned,unsigned,unsigned);
esp_err_t led_strip_refresh(led_strip_handle_t);
esp_err_t led_strip_clear(led_strip_handle_t);
esp_err_t led_strip_del(led_strip_handle_t);
'''
HARNESS=r'''
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include "mock_sdk.h"
#include "gateway_control.h"
#include "gateway_state.h"
#include "gateway_settings.h"
esp_err_t serial_console_start(void){return ESP_OK;}
static unsigned serial_polls;void serial_console_poll(void){serial_polls++;}
static gateway_settings_t settings={.sta_ssid="router",.socks_ipv4="192.168.1.2",.socks_port=7897,.dns_ipv4="1.1.1.1"};
const gateway_settings_t *gateway_settings(void){return &settings;}
static gateway_state_t state={.ap_up=true,.sta_ip=true};
void gateway_state_snapshot(gateway_state_t *out){*out=state;}
static uint64_t clock_ms=1000;
uint64_t gateway_state_clock_ms(void){return clock_ms;}
static bool trial_pending;
bool gateway_settings_pending(void){return trial_pending;}
void gateway_internet_invalidate(void){state.network_generation++;state.internet_checked_ms=0;}
uint32_t gateway_internet_begin(void){return state.network_generation;}
bool gateway_internet_complete(uint32_t generation,int code){if(generation!=state.network_generation)return false;state.internet_failures=code?state.internet_failures+1:0;state.internet_checked_ms=clock_ms;return true;}
uint32_t gateway_internet_retry_ms(unsigned failures){return failures?30000:300000;}
unsigned gateway_state_led_color(const gateway_state_t *s,bool enabled,bool configured,uint64_t now){(void)s;(void)configured;(void)now;return enabled?3:0;}
void gateway_event(gateway_event_kind_t kind,int32_t detail){(void)kind;(void)detail;}
static int stored=-1,staged,writes,fail_commit,led_updates,red,green,blue;
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out){assert(!strcmp(name,"indicators") || !strcmp(name,"healthcfg"));(void)mode;*out=!strcmp(name,"indicators")?1:2;return 0;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *out){(void)h;assert(!strcmp(key,"enabled"));if(stored<0)return ESP_ERR_NVS_NOT_FOUND;*out=stored;return 0;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v){(void)h;assert(!strcmp(key,"enabled"));staged=v;return 0;}
static unsigned char health[4],staged_health[4];static bool health_saved;
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size){assert(h==2 && !strcmp(key,"prefs") && *size==4);if(!health_saved)return ESP_ERR_NVS_NOT_FOUND;memcpy(out,health,4);return 0;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *value,size_t size){assert(h==2 && !strcmp(key,"prefs") && size==4);memcpy(staged_health,value,4);assert(gateway_control_submit(GW_ACTION_DIAGNOSE)==ESP_ERR_INVALID_STATE);return 0;}
esp_err_t nvs_commit(nvs_handle_t h){if(fail_commit)return ESP_FAIL;if(h==1)stored=staged;else{memcpy(health,staged_health,4);health_saved=true;}writes++;return 0;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t led_strip_new_rmt_device(const led_strip_config_t *c,const led_strip_rmt_config_t *r,led_strip_handle_t *out){assert(c->strip_gpio_num==48 && c->max_leds==1 && c->led_model==LED_MODEL_WS2812 && !r->flags.with_dma);*out=(void *)1;return 0;}
esp_err_t led_strip_set_pixel(led_strip_handle_t h,unsigned pixel,unsigned r,unsigned g,unsigned b){(void)h;assert(!pixel);red=r;green=g;blue=b;return 0;}
esp_err_t led_strip_refresh(led_strip_handle_t h){(void)h;led_updates++;return 0;}
esp_err_t led_strip_clear(led_strip_handle_t h){(void)h;return 0;}
esp_err_t led_strip_del(led_strip_handle_t h){(void)h;return 0;}
static void (*task)(void *);
static unsigned idle_once;static bool queued;static gateway_action_t next_action;static jmp_buf stopped;
QueueHandle_t xQueueCreate(unsigned count,unsigned size){assert(count==1 && size==sizeof(gateway_action_t));return (void *)1;}
int xQueueSend(QueueHandle_t q,const void *value,unsigned ticks){(void)q;assert(!ticks && !queued);next_action=*(const gateway_action_t *)value;queued=true;return pdTRUE;}
int xQueueReceive(QueueHandle_t q,void *value,unsigned ticks){(void)q;(void)ticks;if(!queued){if(idle_once){idle_once--;return 0;}longjmp(stopped,1);}*(gateway_action_t *)value=next_action;queued=false;return pdTRUE;}
void vQueueDelete(QueueHandle_t q){(void)q;}
int xTaskCreate(void (*f)(void *),const char *name,unsigned stack,void *arg,unsigned priority,void *handle){(void)name;(void)arg;(void)handle;assert(stack==6144 && priority==2);task=f;return pdPASS;}
void vTaskDelay(unsigned ticks){(void)ticks;}
unsigned uxTaskGetStackHighWaterMark(void *arg){(void)arg;return 3000;}
static int disconnects,connects,resets_tcp,resets_udp,reboots,probes,recoveries,recovery_error;
static bool recovered_network;
esp_err_t gateway_settings_recover(bool network,char ip[16]){recoveries++;recovered_network=network;if(recovery_error)return recovery_error;strcpy(ip,"192.168.4.1");return 0;}
esp_err_t esp_wifi_disconnect(void){disconnects++;return ESP_ERR_WIFI_NOT_CONNECT;}
esp_err_t esp_wifi_connect(void){connects++;return 0;}
void socks_relay_reset_client(void){resets_tcp++;}
void udp_gateway_reset_client(void){resets_udp++;}
void esp_restart(void){reboots++;longjmp(stopped,1);}
void gateway_probe_run(const char *host,uint16_t port,const char *dns,gateway_probe_result_t *out,void (*idle)(void)){assert(!strcmp(host,settings.socks_ipv4) && port==7897 && !strcmp(dns,settings.dns_ipv4));probes++;*out=(gateway_probe_result_t){.code={0,0,0}};unsigned before=serial_polls;idle();assert(serial_polls==before+1);}
static unsigned internet_runs;static int internet_error;static bool stale;
void gateway_internet_run(const char *host,uint16_t port,const char *dns,unsigned target,gateway_internet_result_t *out,void (*idle)(void)){assert(!strcmp(host,settings.socks_ipv4) && port==7897 && !strcmp(dns,settings.dns_ipv4) && target<2);internet_runs++;*out=(gateway_internet_result_t){.http_code=internet_error};if(stale)gateway_internet_invalidate();unsigned before=serial_polls;idle();assert(serial_polls==before+1);}
static void run_task(void){if(!setjmp(stopped))task(0);}
int main(void){
    gateway_control_snapshot_t s;
    assert(gateway_control_start()==ESP_OK);gateway_control_snapshot(&s);
    assert(s.available && s.led_enabled && s.led_ready && s.led_color==3);
    assert(red==0 && green==8 && blue==0);
    fail_commit=1;assert(gateway_control_set_led(false)==ESP_FAIL);gateway_control_snapshot(&s);assert(s.led_enabled);fail_commit=0;
    assert(gateway_control_set_led(false)==ESP_OK && stored==0 && writes==1);
    int before=led_updates;assert(gateway_control_set_led(false)==ESP_OK && writes==1 && led_updates==before); /* No HTTP driver work. */
    assert(gateway_control_start()==ESP_OK);gateway_control_snapshot(&s);assert(!s.led_enabled && s.led_color==0 && !red && !green && !blue); /* Boot clears even when preference is off. */
    assert(gateway_control_submit(GW_ACTION_DIAGNOSE)==ESP_OK);
    assert(gateway_control_submit(GW_ACTION_REBOOT)==ESP_ERR_INVALID_STATE);run_task();gateway_control_snapshot(&s);assert(!s.running && s.action==GW_ACTION_DIAGNOSE && s.probe.code[2]==0 && probes==1 && s.stack_min==3000);
    assert(gateway_control_submit(GW_ACTION_RELAY_RESET)==ESP_OK);run_task();assert(resets_tcp==1 && resets_udp==1);
    assert(gateway_control_submit(GW_ACTION_RECONNECT)==ESP_OK);run_task();assert(disconnects==1 && connects==1);
    state.sta_ip=false;assert(gateway_control_submit(GW_ACTION_DIAGNOSE)==ESP_OK);run_task();gateway_control_snapshot(&s);assert(s.result==ESP_ERR_INVALID_STATE && probes==1);
    assert(gateway_control_submit(GW_ACTION_NONE)==ESP_ERR_INVALID_ARG);
    state.sta_ip=true;
    gateway_control_snapshot(&s);assert(s.internet_auto && s.internet_target==0);
    fail_commit=1;assert(gateway_control_set_internet(false,1)==ESP_FAIL);fail_commit=0;
    gateway_control_snapshot(&s);assert(s.internet_auto && s.internet_target==0);
    assert(gateway_control_set_internet(false,1)==ESP_OK);int saved_writes=writes;
    assert(gateway_control_set_internet(false,1)==ESP_OK && writes==saved_writes);
    assert(gateway_control_start()==ESP_OK);gateway_control_snapshot(&s);assert(!s.internet_auto && s.internet_target==1);
    assert(gateway_control_submit(GW_ACTION_INTERNET)==ESP_OK);run_task();gateway_control_snapshot(&s);
    assert(internet_runs==1 && !s.result && !s.job_auto && !s.next_check_ms);
    assert(gateway_control_set_internet(true,0)==ESP_OK);
    idle_once=1;run_task();gateway_control_snapshot(&s);assert(s.next_check_ms==clock_ms+2000 && internet_runs==1);
    clock_ms+=2000;idle_once=1;run_task();gateway_control_snapshot(&s);
    assert(serial_polls>0);
    assert(internet_runs==2 && s.job_auto && s.next_check_ms==clock_ms+300000);
    clock_ms+=100;idle_once=1;run_task();assert(internet_runs==2);
    clock_ms+=300000;internet_error=-110;idle_once=1;run_task();gateway_control_snapshot(&s);
    assert(internet_runs==3 && s.result==-110 && s.next_check_ms==clock_ms+30000);
    stale=true;assert(gateway_control_submit(GW_ACTION_INTERNET)==ESP_OK);run_task();gateway_control_snapshot(&s);
    assert(s.result==ESP_ERR_INVALID_STATE);stale=false;idle_once=1;run_task();gateway_control_snapshot(&s);
    assert(s.next_check_ms==clock_ms+2000);
    trial_pending=true;clock_ms+=2000;unsigned previous_runs=internet_runs;idle_once=1;run_task();gateway_control_snapshot(&s);
    assert(internet_runs==previous_runs && !s.next_check_ms);
    trial_pending=false;idle_once=1;run_task();assert(internet_runs==previous_runs+1);
    recovery_error=ESP_FAIL;assert(gateway_control_submit(GW_ACTION_RESET_ADMIN)==ESP_OK);run_task();gateway_control_snapshot(&s);
    assert(recoveries==1 && !recovered_network && s.result==ESP_FAIL && reboots==0);
    recovery_error=0;assert(gateway_control_submit(GW_ACTION_RESET_NETWORK)==ESP_OK);run_task();assert(recoveries==2 && recovered_network && reboots==1);
    assert(gateway_control_start()==ESP_OK);assert(gateway_control_submit(GW_ACTION_RESET_ADMIN)==ESP_OK);run_task();assert(recoveries==3 && !recovered_network && reboots==2);
    assert(gateway_control_start()==ESP_OK);assert(gateway_control_submit(GW_ACTION_REBOOT)==ESP_OK);run_task();assert(reboots==3 && stored==0);return 0;
}
'''
class ControlTests(unittest.TestCase):
    def test_queue_actions_and_led_persistence_commit_failure(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'mock_sdk.h').write_text(SDK);(p/'freertos').mkdir();(p/'lwip').mkdir()
            for f in ['esp_err.h','esp_wifi.h','esp_system.h','esp_log.h','led_strip.h','nvs.h','sdkconfig.h','freertos/FreeRTOS.h','freertos/queue.h','freertos/task.h']:(p/f).write_text('#include "mock_sdk.h"\n')
            for f in ['lwip/pbuf.h','lwip/netif.h']:(p/f).write_text('struct pbuf;struct netif;typedef int err_t;\n')
            (p/'check.c').write_text(HARNESS)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-DESP_PLATFORM','-I',str(p),'-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/gateway_control.c'),'-o',str(p/'check')],check=True)
            subprocess.run([str(p/'check')],check=True)
