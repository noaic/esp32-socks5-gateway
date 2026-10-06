#include "cpu_profile.h"
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <inttypes.h>

#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/* Fixed storage; no new task or per-sample allocation. Only persistent task
 * handles are examined: never dereference names of a deleted ping task. */
static TaskStatus_t snapshot[64];
static const char *const names[] = {"IDLE0", "IDLE1", "wifi", "tcpip", "proxy_tcp", "proxy_udp", "esp_timer", "link_probe", "httpd"};
#define PROFILE_COUNT (sizeof(names)/sizeof(names[0]))
static struct { UBaseType_t id; configRUN_TIME_COUNTER_TYPE counter; bool valid; } previous[PROFILE_COUNT];
static configRUN_TIME_COUNTER_TYPE previous_time;
#endif

void cpu_profile_log(void)
{
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS && CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    TaskHandle_t handles[PROFILE_COUNT];
    for (unsigned i=0;i<PROFILE_COUNT;i++) handles[i]=xTaskGetHandle(names[i]);
    configRUN_TIME_COUNTER_TYPE now=0;
    int64_t start=esp_timer_get_time();
    UBaseType_t count=uxTaskGetSystemState(snapshot,64,&now);
    int64_t cost=esp_timer_get_time()-start;
    if (!count) {
        ESP_LOGW("cpu_profile","snapshot capacity exceeded; sample skipped");
        previous_time=0;
        for (unsigned i=0;i<PROFILE_COUNT;i++) previous[i].valid=false;
        return;
    }
    uint64_t elapsed=previous_time && now>previous_time ? now-previous_time : 0;
    ESP_LOGI("cpu_profile","window_us=%" PRIu64 " tasks=%u snapshot_us=%" PRId64 " (100%%=one core)",elapsed,(unsigned)count,cost);
    for (unsigned i=0;i<PROFILE_COUNT;i++) {
        bool found=false;
        for (UBaseType_t j=0;j<count;j++) {
            TaskStatus_t *task=&snapshot[j];
            if (!handles[i] || task->xHandle!=handles[i]) continue;
            found=true;
            if (elapsed && previous[i].valid && previous[i].id==task->xTaskNumber && task->ulRunTimeCounter>=previous[i].counter) {
                uint64_t runtime=task->ulRunTimeCounter-previous[i].counter;
                unsigned permille=(unsigned)(runtime*1000/elapsed);
                ESP_LOGI("cpu_profile","task=%s cpu=%u.%u%% runtime_us=%" PRIu64 " core=%d",names[i],permille/10,permille%10,runtime,(int)xTaskGetCoreID(task->xHandle));
            }
            previous[i].id=task->xTaskNumber;
            previous[i].counter=task->ulRunTimeCounter;
            previous[i].valid=true;
            break;
        }
        if (!found) previous[i].valid=false;
    }
    previous_time=now;
#endif
}
