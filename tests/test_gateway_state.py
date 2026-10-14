"""Production state transitions, bounded history, concurrency and indicator policy."""
import pathlib
import subprocess
import tempfile
import unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
HARNESS=r'''
#include "gateway_state.h"
#include <assert.h>
#include <pthread.h>
#include <string.h>
static void *writer(void *arg) {
    for(unsigned i=0;i<250;i++)gateway_event(GW_TCP_ERROR,(int)(intptr_t)arg);
    return 0;
}
int main(void) {
    gateway_state_t s;gateway_event_t history[64];
    gateway_event(GW_BOOT,1);gateway_event(GW_AP_UP,0);gateway_event(GW_STA_LINK,6);gateway_event(GW_STA_IP,0);
    gateway_state_snapshot(&s);assert(s.ap_up && s.sta_link && s.sta_ip && !s.subnet_conflict);
    assert(gateway_state_led_color(&s,true,true,gateway_state_clock_ms())==5);
    uint32_t generation=gateway_internet_begin();gateway_state_snapshot(&s);
    assert(gateway_internet_status(&s,gateway_state_clock_ms())==GW_INTERNET_CHECKING);
    assert(gateway_internet_complete(generation,0));gateway_state_snapshot(&s);
    assert(gateway_state_led_color(&s,true,true,s.internet_checked_ms)==3);
    assert(gateway_internet_status(&s,s.internet_checked_ms+GW_INTERNET_VALID_MS)==GW_INTERNET_EXPIRED);
    assert(gateway_state_led_color(&s,true,true,s.internet_checked_ms+GW_INTERNET_VALID_MS)==5);
    assert(gateway_internet_complete(gateway_internet_begin(),-110));gateway_state_snapshot(&s);
    assert(s.internet_failures==1 && gateway_state_led_color(&s,true,true,s.internet_checked_ms)==5);
    assert(gateway_internet_complete(gateway_internet_begin(),-110));gateway_state_snapshot(&s);
    assert(s.internet_failures==2 && gateway_state_led_color(&s,true,true,s.internet_checked_ms)==4);
    assert(gateway_internet_complete(gateway_internet_begin(),0));gateway_state_snapshot(&s);
    assert(!s.internet_failures && gateway_state_led_color(&s,true,true,s.internet_checked_ms)==3);
    generation=gateway_internet_begin();gateway_event(GW_STA_DOWN,201);gateway_event(GW_STA_IP,0);
    assert(!gateway_internet_complete(generation,0));gateway_state_snapshot(&s);
    assert(!s.internet_checked_ms && gateway_state_led_color(&s,true,true,gateway_state_clock_ms())==5);
    assert(gateway_internet_retry_ms(0)==300000 && gateway_internet_retry_ms(1)==30000);
    assert(gateway_internet_retry_ms(2)==60000 && gateway_internet_retry_ms(3)==120000);
    assert(gateway_internet_retry_ms(4)==240000 && gateway_internet_retry_ms(5)==300000);
    assert(gateway_internet_retry_ms(1000)==300000);
    gateway_event(GW_SUBNET_CONFLICT,0);gateway_state_snapshot(&s);
    assert(gateway_state_led_color(&s,true,true,gateway_state_clock_ms())==4);
    assert(gateway_state_led_color(&s,false,true,gateway_state_clock_ms())==0);
    gateway_event(GW_STA_DOWN,201);gateway_state_snapshot(&s);
    assert(!s.sta_ip && !s.sta_link && !s.subnet_conflict && s.sta_reason==201);
    assert(gateway_state_led_color(&s,true,true,gateway_state_clock_ms())==2);
    assert(gateway_state_led_color(&s,true,false,gateway_state_clock_ms())==1);
    gateway_event(GW_STA_IP,0);gateway_tcp_connected();gateway_tcp_response();gateway_udp_reply(true);
    gateway_state_snapshot(&s);assert(s.tcp_connect_ms && s.tcp_data_ms && s.dns_reply_ms==s.udp_reply_ms);
    gateway_event(GW_TCP_ERROR,1007);gateway_event(GW_TCP_ERROR,1007);gateway_event(GW_TCP_ERROR,1007);
    gateway_state_snapshot(&s);assert(s.tcp_error_streak==3 && s.tcp_errors==3);
    size_t n=gateway_events_snapshot(history,64);assert(history[n-1].count==3 && history[n-1].detail==1007);
    assert(gateway_state_led_color(&s,true,true,s.tcp_error_ms+1000)==5);
    assert(gateway_state_led_color(&s,true,true,s.tcp_error_ms+30001)==5);
    gateway_tcp_response();gateway_state_snapshot(&s);assert(!s.tcp_error_streak);
    for(unsigned i=0;i<100;i++)gateway_event(GW_CLIENT_JOIN,(int)i);
    assert(gateway_events_snapshot(history,64)==64);assert(history[0].detail==36 && history[63].detail==99);
    assert(gateway_events_snapshot(history,2)==2 && history[0].detail==98);
    pthread_t workers[4];for(unsigned i=0;i<4;i++)assert(!pthread_create(&workers[i],0,writer,(void *)(intptr_t)i));
    for(unsigned i=0;i<4;i++)pthread_join(workers[i],0);
    gateway_state_snapshot(&s);assert(s.tcp_errors==1003);
    n=gateway_events_snapshot(history,64);assert(n<=64);
    for(size_t i=1;i<n;i++)assert(history[i].sequence>history[i-1].sequence && history[i].last_ms>=history[i-1].last_ms);
    assert(!strcmp(gateway_event_name((gateway_event_kind_t)999),"未知事件"));
    assert(!strcmp(gateway_event_code(GW_BOOT),"boot"));
    assert(!strcmp(gateway_event_code(GW_RECOVERY_COMMIT),"recovery_commit"));
    assert(!strcmp(gateway_event_code((gateway_event_kind_t)999),"unknown"));
    return 0;
}
'''
class StateTests(unittest.TestCase):
    def test_transitions_ring_concurrency_and_led_policy(self):
        with tempfile.TemporaryDirectory() as d:
            p=pathlib.Path(d);(p/'check.c').write_text(HARNESS)
            subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/gateway_state.c'),'-o',str(p/'check')],check=True)
            subprocess.run([str(p/'check')],check=True)
