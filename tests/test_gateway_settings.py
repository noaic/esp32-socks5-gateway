"""Execute production settings validation, bounded JSON patching and NVS trial lifecycle."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
JSON_DIR = ROOT / 'managed_components/espressif__cjson/cJSON'
VALIDATION = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "management_request.h"
static gateway_settings_t baseline(void) {
    gateway_settings_t s={.sta_ssid="router",.sta_password="upstream123",
        .ap_ssid="ESP32",.ap_password="ap-password",.socks_ipv4="192.168.1.2",
        .dns_ipv4="1.1.1.1",.admin_password="management123",.socks_port=7897,.max_clients=2,.ap_subnet=4};
    return s;
}
static bool patch(const char *json,gateway_settings_t *s) {
    cJSON *root=management_json_parse(json,strlen(json));
    if (!root) return false;
    bool ok=management_settings_patch(root,s); cJSON_Delete(root); return ok;
}
int main(void) {
    gateway_settings_t s=baseline(); assert(gateway_settings_valid(&s));
    char address[16]; gateway_settings_ap_ip(&s,address); assert(!strcmp(address,"192.168.4.1"));
    assert(patch("{\"ap_subnet\":10}",&s));
    gateway_settings_ap_ip(&s,address); assert(!strcmp(address,"192.168.10.1"));
    assert(patch("{\"dns_ipv4\":\"192.168.4.2\"}",&s)); /* Old AP subnet is now upstream-valid. */
    assert(!patch("{\"socks_ipv4\":\"192.168.10.77\"}",&s));
    s=baseline();
    assert(patch("{\"ap_subnet\":0}",&s)); gateway_settings_ap_ip(&s,address); assert(!strcmp(address,"192.168.0.1"));
    assert(patch("{\"ap_subnet\":255}",&s)); gateway_settings_ap_ip(&s,address); assert(!strcmp(address,"192.168.255.1"));
    s=baseline();
    assert(gateway_settings_ap_overlap(&s,0xc0a80477,0xffffff00)); /* equal /24 */
    assert(gateway_settings_ap_overlap(&s,0xc0a80490,0xffffff80)); /* contained /25 */
    assert(gateway_settings_ap_overlap(&s,0xc0a80577,0xfffffe00)); /* enclosing /23 */
    assert(gateway_settings_ap_overlap(&s,0xc0a87c77,0xffff0000)); /* enclosing /16 */
    assert(!gateway_settings_ap_overlap(&s,0xc0a80577,0xffffff00));
    assert(!gateway_settings_ap_overlap(&s,0,0)); /* disconnected */
    assert(patch("{\"max_clients\":1,\"socks_port\":65535}",&s));
    assert(s.max_clients==1 && s.socks_port==65535);
    assert(patch("{\"sta_password\":\"\",\"ap_password\":\"\",\"admin_password\":\"\"}",&s));
    assert(!strcmp(s.sta_password,"upstream123") && !strcmp(s.admin_password,"management123"));
    assert(patch("{\"sta_open\":true,\"sta_password\":\"ignored-value\"}",&s));
    assert(!s.sta_password[0]);
    const char *invalid[]={"{\"ap_subnet\":-1}","{\"ap_subnet\":256}","{\"ap_subnet\":4.5}","{\"ap_subnet\":\"5\"}","{\"ap_subnet\":true}","{\"ap_subnet\":1}","{}","[]","null","{\"max_clients\":3}",
        "{\"max_clients\":1.5}","{\"max_clients\":\"1\"}","{\"socks_port\":0}",
        "{\"socks_port\":65536}","{\"socks_port\":1e100}","{\"max_clients\":2,\"max_clients\":1}",
        "{\"unknown\":1}","{\"sta_open\":1}","{\"ap_password\":\"short\"}",
        "{\"admin_password\":\"short\"}","{\"sta_password\":\"short\"}",
        "{\"sta_ssid\":\"a\\u0000b\"}","{\"sta_ssid\":\"a\\nb\"}",
        "{\"sta_ssid\":\"12345678901234567890123456789012345\"}",
        "{\"dns_ipv4\":\"192.168.4.1\"}","{\"socks_ipv4\":\"127.0.0.1\"}",
        "{\"socks_ipv4\":\"192.168.1.077\"}","{\"dns_ipv4\":\"224.0.0.1\"}",
        "{\"socks_ipv4\":\"example.com\"}","{\"socks_ipv4\":\"1.2.3.256\"}",
        "{\"a\":{\"b\":{\"c\":1}}}","{\"sta_ssid\":\"x\"} trailing"};
    for (unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++) { s=baseline(); assert(!patch(invalid[i],&s)); }
    s=baseline(); memset(s.sta_password,'a',64); s.sta_password[64]=0;
    assert(gateway_settings_valid(&s)); s.sta_password[63]='g'; assert(!gateway_settings_valid(&s));
    s=baseline(); memset(s.ap_ssid,'a',sizeof(s.ap_ssid)); assert(!gateway_settings_valid(&s));
    char oversized[1100]; memset(oversized,' ',sizeof(oversized)); oversized[1099]=0;
    assert(!management_json_parse(oversized,1099));
    const char embedded[]="{\"sta_ssid\":\"x\0y\"}";
    assert(!management_json_parse(embedded,sizeof(embedded)-1));
    puts("settings validation and JSON patch boundaries passed");
}
'''
SDK = r'''
#ifndef SDK_MOCK_H
#define SDK_MOCK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
typedef int esp_err_t;
typedef pthread_mutex_t StaticSemaphore_t;
typedef pthread_mutex_t *SemaphoreHandle_t;
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffu
#define ESP_ERR_NO_MEM 5
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *p){return pthread_mutex_init(p,NULL)?NULL:p;}
static inline int xSemaphoreTake(SemaphoreHandle_t p,unsigned ticks){(void)ticks;return pthread_mutex_lock(p)==0;}
static inline int xSemaphoreGive(SemaphoreHandle_t p){return pthread_mutex_unlock(p)==0;}
typedef unsigned nvs_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NVS_NOT_FOUND 3
#define ESP_ERR_NVS_INVALID_LENGTH 4
#define NVS_READWRITE 1
#define CONFIG_PROXY_STA_SSID "router"
#define CONFIG_PROXY_STA_PASSWORD "upstream123"
#define CONFIG_PROXY_AP_SSID "ESP32"
#define CONFIG_PROXY_AP_PASSWORD "ap-password"
#define ESP_LOGI(tag,...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag,...) do { (void)(tag); } while (0)
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
void nvs_close(nvs_handle_t);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_commit(nvs_handle_t);
void esp_fill_random(void *,size_t);
void bootloader_random_enable(void);
void bootloader_random_disable(void);
#endif
'''
LIFECYCLE = r'''
#include <assert.h>
#include <string.h>
#include "mock_sdk.h"
#include "gateway_settings.h"
#include <stddef.h>
#include "gateway_state.h"
void gateway_event(gateway_event_kind_t kind,int32_t detail) {(void)kind;(void)detail;}
static unsigned char persisted[4096], staged[4096];
static size_t persistent_size,stage_size;
static bool fail_commit,entropy;
/* Old record with opaque trailing padding in each settings struct. */
typedef struct {uint32_t version;bool bootstrap,previous_bootstrap;uint8_t trial;gateway_settings_t active,previous;} legacy_record_layout;
static void mark_v1(void) {
    assert(persistent_size==600);
    uint32_t v=1; memcpy(persisted,&v,sizeof(v));
    persisted[offsetof(legacy_record_layout,active)+offsetof(gateway_settings_t,ap_subnet)]=0xa5;
    persisted[offsetof(legacy_record_layout,previous)+offsetof(gateway_settings_t,ap_subnet)]=0x5a;
}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *out) {(void)name;(void)mode;*out=1;return ESP_OK;}
void nvs_close(nvs_handle_t h) {(void)h;stage_size=0;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n) {
    (void)h;(void)key;if(!persistent_size)return ESP_ERR_NVS_NOT_FOUND;
    if(*n<persistent_size)return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out,persisted,persistent_size);*n=persistent_size;return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t n) {
    (void)h;(void)key;assert(n<=sizeof(staged));memcpy(staged,in,n);stage_size=n;return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h;if(fail_commit)return ESP_FAIL;
    memcpy(persisted,staged,stage_size);persistent_size=stage_size;return ESP_OK;
}
void bootloader_random_enable(void) {assert(!entropy);entropy=true;}
void bootloader_random_disable(void) {assert(entropy);entropy=false;}
void esp_fill_random(void *p,size_t n) {assert(entropy);memset(p,0xa5,n);}
int main(void) {
    assert(gateway_settings_init()==ESP_OK && !entropy);
    assert(gateway_settings_needs_setup() && !gateway_settings()->admin_password[0]);
    assert(gateway_settings_init()==ESP_OK && gateway_settings_needs_setup()); /* RST never claims it. */
    gateway_settings_t unclaimed=*gateway_settings(); strcpy(unclaimed.admin_password,"bypass-attempt");
    assert(gateway_settings_stage(&unclaimed)==ESP_ERR_INVALID_STATE);
    assert(gateway_settings_setup_password("short")==ESP_ERR_INVALID_ARG);
    fail_commit=true;
    assert(gateway_settings_setup_password("personal-password")==ESP_FAIL);
    assert(gateway_settings_needs_setup() && !gateway_settings()->admin_password[0]);
    fail_commit=false;
    assert(gateway_settings_setup_password("personal-password")==ESP_OK);
    assert(!gateway_settings_needs_setup());
    assert(gateway_settings_setup_password("another-password")==ESP_ERR_INVALID_STATE);
    gateway_settings_t initial=*gateway_settings();
    assert(!strcmp(initial.admin_password,"personal-password"));
    assert(gateway_settings_init()==ESP_OK && !gateway_settings_needs_setup() && !strcmp(gateway_settings()->admin_password,initial.admin_password));
    assert(initial.ap_subnet==4);
    mark_v1(); fail_commit=true; assert(gateway_settings_init()==ESP_FAIL); fail_commit=false;
    assert(gateway_settings_init()==ESP_OK && !gateway_settings_needs_setup());
    assert(gateway_settings()->ap_subnet==4 && !strcmp(gateway_settings()->admin_password,initial.admin_password));
    gateway_settings_t next=initial;strcpy(next.socks_ipv4,"192.168.1.88");next.max_clients=1;next.ap_subnet=10;
    fail_commit=true;assert(gateway_settings_stage(&next)==ESP_FAIL);fail_commit=false;
    assert(gateway_settings_init()==ESP_OK && !gateway_settings_pending());
    assert(!strcmp(gateway_settings()->socks_ipv4,initial.socks_ipv4));
    assert(gateway_settings_stage(&next)==ESP_OK);
    assert(gateway_settings_stage(&next)==ESP_ERR_INVALID_STATE);
    assert(!strcmp(gateway_settings()->socks_ipv4,initial.socks_ipv4)); /* No live mutation. */
    mark_v1(); /* Trial migration retains the old network and pending flag. */
    assert(gateway_settings_init()==ESP_OK && gateway_settings_pending());
    assert(gateway_settings()->max_clients==1 && gateway_settings()->ap_subnet==4);
    assert(gateway_settings_init()==ESP_OK && !gateway_settings_pending()); /* Reset before confirm. */
    assert(gateway_settings()->max_clients==2);
    assert(gateway_settings_stage(&next)==ESP_OK && gateway_settings_init()==ESP_OK);
    fail_commit=true;assert(gateway_settings_confirm()==ESP_FAIL && gateway_settings_pending());fail_commit=false;
    assert(gateway_settings_confirm()==ESP_OK && !gateway_settings_pending());
    assert(gateway_settings_init()==ESP_OK && gateway_settings()->max_clients==1 && gateway_settings()->ap_subnet==10);
    gateway_settings_t saved=*gateway_settings();next=saved;next.max_clients=2;
    strcpy(next.admin_password,"a-different-password");
    assert(gateway_settings_stage(&next)==ESP_OK && gateway_settings_init()==ESP_OK);
    assert(!strcmp(gateway_settings()->admin_password,"a-different-password"));
    assert(gateway_settings_rollback()==ESP_OK && gateway_settings_init()==ESP_OK);
    assert(!strcmp(gateway_settings()->admin_password,saved.admin_password));
    assert(gateway_settings()->max_clients==1);
    next=saved;next.socks_port=0;
    assert(gateway_settings_stage(&next)==ESP_ERR_INVALID_ARG);
    next=saved;next.max_clients=2;next.ap_subnet=20;
    assert(gateway_settings_stage(&next)==ESP_OK && gateway_settings_init()==ESP_OK);
    fail_commit=true;assert(gateway_settings_rollback()==ESP_FAIL);fail_commit=false;
    assert(gateway_settings_init()==ESP_OK && gateway_settings()->max_clients==1);
    assert(gateway_settings()->ap_subnet==10);
    puts("NVS atomic commit, trial, confirmation and rollback passed");
}
'''
class SettingsTests(unittest.TestCase):
    def test_configuration_and_json_boundaries(self):
        with tempfile.TemporaryDirectory(prefix='gateway-schema-') as directory:
            p=pathlib.Path(directory); (p/'check.c').write_text(VALIDATION)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',
                '-I',str(ROOT/'main'),'-I',str(JSON_DIR),str(p/'check.c'),str(ROOT/'main/settings_validation.c'),
                str(ROOT/'main/management_request.c'),str(JSON_DIR/'cJSON.c'),'-lm','-o',str(p/'check')],check=True)
            subprocess.run([str(p/'check')],check=True)
    def test_nvs_trial_and_commit_failures(self):
        with tempfile.TemporaryDirectory(prefix='gateway-nvs-') as directory:
            p=pathlib.Path(directory);(p/'mock_sdk.h').write_text(SDK)
            for header in ('esp_err.h','sdkconfig.h','esp_log.h','esp_random.h','bootloader_random.h','nvs.h'):
                (p/header).write_text('#include "mock_sdk.h"\n')
            (p/'freertos').mkdir()
            for header in ('FreeRTOS.h','semphr.h'):(p/'freertos'/header).write_text('#include "mock_sdk.h"\n')
            (p/'check.c').write_text(LIFECYCLE)
            subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pthread','-DESP_PLATFORM',
                '-I',str(p),'-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/gateway_settings.c'),
                str(ROOT/'main/settings_validation.c'),'-o',str(p/'check')],check=True)
            subprocess.run([str(p/'check')],check=True)
if __name__=='__main__':unittest.main()
