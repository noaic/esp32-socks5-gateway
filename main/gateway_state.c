#include "gateway_state.h"
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
static portMUX_TYPE state_lock=portMUX_INITIALIZER_UNLOCKED;
#define LOCK() portENTER_CRITICAL(&state_lock)
#define UNLOCK() portEXIT_CRITICAL(&state_lock)
uint64_t gateway_state_clock_ms(void) { return (uint64_t)esp_timer_get_time()/1000; }
#else
#include <pthread.h>
#include <time.h>
static pthread_mutex_t state_lock=PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&state_lock)
#define UNLOCK() pthread_mutex_unlock(&state_lock)
uint64_t gateway_state_clock_ms(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
#endif
static gateway_state_t state;
static gateway_event_t events[GATEWAY_EVENT_CAPACITY];
static unsigned head,used;
static void invalidate_locked(void)
{
    state.network_generation++;
    state.internet_checking=false;state.internet_failures=0;
    state.internet_checked_ms=state.internet_success_ms=0;
}
static uint32_t sequence;
void gateway_event(gateway_event_kind_t kind,int32_t detail)
{
    LOCK();
    uint64_t now=gateway_state_clock_ms();
    if(kind==GW_AP_UP || kind==GW_AP_DOWN || kind==GW_STA_IP || kind==GW_STA_DOWN ||
       kind==GW_STA_LOST_IP || kind==GW_SUBNET_CONFLICT)invalidate_locked();
    switch (kind) {
    case GW_AP_UP: state.ap_up=true;break;
    case GW_AP_DOWN: state.ap_up=false;break;
    case GW_STA_LINK: state.sta_link=true;break;
    case GW_STA_IP: state.sta_link=state.sta_ip=true;state.subnet_conflict=false;break;
    case GW_SUBNET_CONFLICT: state.sta_link=state.sta_ip=true;state.subnet_conflict=true;break;
    case GW_STA_DOWN: state.sta_link=state.sta_ip=false;state.subnet_conflict=false;state.sta_reason=detail;break;
    case GW_STA_LOST_IP: state.sta_ip=false;state.subnet_conflict=false;break;
    case GW_TCP_ERROR:
        if (now-state.tcp_error_ms>30000) state.tcp_error_streak=0;
        state.tcp_error_ms=now;state.tcp_error=detail;state.tcp_errors++;state.tcp_error_streak++;break;
    case GW_UDP_ERROR: state.udp_error_ms=now;state.udp_error=detail;state.udp_errors++;break;
    default: break;
    }
    unsigned last=(head+GATEWAY_EVENT_CAPACITY-1)%GATEWAY_EVENT_CAPACITY;
    if (used && events[last].kind==kind && events[last].detail==detail && now-events[last].last_ms<10000) {
        events[last].last_ms=now;events[last].count++;
    } else {
        events[head]=(gateway_event_t){.first_ms=now,.last_ms=now,.sequence=++sequence,.count=1,.detail=detail,.kind=kind};
        head=(head+1)%GATEWAY_EVENT_CAPACITY;if(used<GATEWAY_EVENT_CAPACITY)used++;
    }
    UNLOCK();
}
void gateway_state_snapshot(gateway_state_t *out) { LOCK();*out=state;UNLOCK(); }
size_t gateway_events_snapshot(gateway_event_t *out,size_t capacity)
{
    LOCK();size_t n=used<capacity?used:capacity;
    for(size_t i=0;i<n;i++)out[i]=events[(head+GATEWAY_EVENT_CAPACITY-(unsigned)n+(unsigned)i)%GATEWAY_EVENT_CAPACITY];
    UNLOCK();return n;
}
void gateway_tcp_connected(void) { uint64_t now=gateway_state_clock_ms();LOCK();state.tcp_connect_ms=now;UNLOCK(); }
void gateway_tcp_response(void) { uint64_t now=gateway_state_clock_ms();LOCK();state.tcp_data_ms=now;state.tcp_error_streak=0;UNLOCK(); }
void gateway_udp_reply(bool dns) { uint64_t now=gateway_state_clock_ms();LOCK();state.udp_reply_ms=now;if(dns)state.dns_reply_ms=now;UNLOCK(); }
const char *gateway_event_name(gateway_event_kind_t kind)
{
    static const char *const names[]={"设备启动","AP 启动","AP 停止","STA 已关联","STA 获得 IP","STA 断开",
        "STA 丢失 IP","网段冲突，转发暂停","客户端接入","客户端离开","配置已保存","配置已确认","配置回滚",
        "首次管理口令已设置","维护开始","维护结束","诊断完成","TCP 上游失败","UDP 关联失败","状态灯设置","状态灯错误","互联网检查完成","互联网检查设置","串口恢复已提交"};
    return (unsigned)kind<sizeof(names)/sizeof(*names)?names[kind]:"未知事件";
}
unsigned gateway_state_led_color(const gateway_state_t *s,bool enabled,bool configured,uint64_t now)
{
    if(!enabled)return 0;
    if(s->subnet_conflict)return 4;
    if(s->ap_up && s->sta_ip) {
        unsigned status=gateway_internet_status(s,now);
        return status==GW_INTERNET_PASS?3:status==GW_INTERNET_FAIL?4:5;
    }
    return configured?2:1;
}
void gateway_internet_invalidate(void){LOCK();invalidate_locked();UNLOCK();}
uint32_t gateway_internet_begin(void)
{LOCK();state.internet_checking=true;uint32_t generation=state.network_generation;UNLOCK();return generation;}
bool gateway_internet_complete(uint32_t generation,int code)
{
    LOCK();
    if(generation!=state.network_generation || !state.ap_up || !state.sta_ip || state.subnet_conflict){UNLOCK();return false;}
    state.internet_checking=false;state.internet_checked_ms=gateway_state_clock_ms();
    if(code){if(state.internet_failures<1000)state.internet_failures++;}
    else {state.internet_success_ms=state.internet_checked_ms;state.internet_failures=0;}
    UNLOCK();gateway_event(GW_INTERNET_RESULT,code);return true;
}
unsigned gateway_internet_status(const gateway_state_t *s,uint64_t now)
{
    if(!s->ap_up || !s->sta_ip || s->subnet_conflict)return GW_INTERNET_UNKNOWN;
    if(s->internet_checking)return GW_INTERNET_CHECKING;
    if(!s->internet_checked_ms)return GW_INTERNET_UNKNOWN;
    if(now-s->internet_checked_ms>=GW_INTERNET_VALID_MS)return GW_INTERNET_EXPIRED;
    if(s->internet_failures)return s->internet_failures>=2?GW_INTERNET_FAIL:GW_INTERNET_UNKNOWN;
    return GW_INTERNET_PASS;
}
uint32_t gateway_internet_retry_ms(unsigned failures)
{return failures>=5?300000u:failures?30000u<<(failures-1):300000u;}
