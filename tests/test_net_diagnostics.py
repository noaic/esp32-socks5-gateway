"""Verify production link wrappers preserve the driver/PCB call contract.
Minimal SDK declarations let the real diagnostic source run on the host.
"""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
API = r'''
#ifndef MOCK_API_H
#define MOCK_API_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define IRAM_ATTR
#define ESP_OK 0
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_ESP_NETIF_TX_FAILED 0x500e
#define ESP_LOGI(tag,...) do { (void)(tag); printf(__VA_ARGS__); puts(""); } while (0)
typedef int esp_err_t;
typedef struct { unsigned id; } esp_netif_t;
esp_err_t esp_netif_transmit_wrap(esp_netif_t *,void *,size_t,void *);
esp_err_t esp_netif_receive(esp_netif_t *,void *,size_t,void *);
void esp_netif_free_rx_buffer(void *,void *);
#define CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 48
void esp_netif_netstack_buf_ref(void *);
void esp_netif_netstack_buf_free(void *);
int64_t esp_timer_get_time(void);
enum { WIFI_IF_STA=0,WIFI_IF_AP=1 };
typedef void (*wifi_tx_done_cb_t)(uint8_t,uint8_t *,uint16_t *,bool);
esp_err_t esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t);
#define TF_INFR 0x04
enum tcp_state { CLOSED,LISTEN,SYN_SENT,SYN_RCVD,ESTABLISHED,FIN_WAIT_1 };
struct tcp_pcb { uint16_t local_port,remote_port; unsigned flags; void *unacked; enum tcp_state state; };
void tcp_rexmit_rto_commit(struct tcp_pcb *);
void tcp_rexmit_fast(struct tcp_pcb *);
typedef enum { MEMP_TCP_PCB,MEMP_UDP_PCB } memp_t;
#define MEMP_NUM_TCP_PCB 256
void *memp_malloc(memp_t);
void memp_free(memp_t,void *);
#endif
'''
HARNESS = r'''
#include "net_diagnostics.h"
#include "mock_api.h"
#include "proxy_config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static esp_netif_t ap={1},sta={2},other={3};
static esp_netif_t *expected_netif;
static void *expected_data,*expected_pbuf;
static size_t expected_len;
static int driver_result;
static unsigned driver_calls,rto_calls,fast_calls,rx_calls,ref_calls,free_calls,rx_frees;
static int held;
static wifi_tx_done_cb_t done_callback;
static bool fast_busy;
static bool inject_done_during_clock;
static struct tcp_pcb *expected_pcb;
static memp_t expected_pool;
static void *pool_result;
static unsigned pool_alloc_calls,pool_free_calls;
static unsigned clock_calls,observer_calls;

void *memp_malloc(memp_t type)
{ assert(type==expected_pool); pool_alloc_calls++; return pool_result; }
void memp_free(memp_t type,void *memory)
{ assert(type==expected_pool && memory==pool_result); pool_free_calls++; }
void expect_pool(memp_t type,void *result) { expected_pool=type; pool_result=result; }

/* Definitions act as real APIs; callers in a separate object go through
 * GNU --wrap, exactly as wlanif/tcp.c/tcp_in.c do in the firmware link. */
esp_err_t esp_netif_transmit_wrap(esp_netif_t *n,void *data,size_t len,void *pbuf)
{
    assert(n==expected_netif && data==expected_data && len==expected_len && pbuf==expected_pbuf);
    driver_calls++; return driver_result;
}
esp_err_t esp_netif_receive(esp_netif_t *n,void *data,size_t len,void *buffer)
{
    assert(n==expected_netif && data==expected_data && len==expected_len && buffer==expected_pbuf);
    rx_calls++;
    /* Emulate RX ownership consumption: a wrapper must not touch this again. */
    free(data);
    return driver_result;
}
void esp_netif_netstack_buf_ref(void *p) { ref_calls++; if (p) held++; }
void esp_netif_free_rx_buffer(void *n,void *p)
{ assert(n==expected_netif && p==expected_pbuf); rx_frees++; }
void esp_netif_netstack_buf_free(void *p) { free_calls++; if (p) held--; }
int64_t esp_timer_get_time(void)
{
    clock_calls++;
    static int64_t time=1000000;
    int64_t sampled=(time+=1000);
    if (inject_done_during_clock) {
        inject_done_during_clock=false;
        uint8_t frame[24]={0x88}; frame[4]=2; uint16_t length=24;
        done_callback(WIFI_IF_STA,frame,&length,true);
    }
    return sampled;
}
esp_err_t esp_wifi_set_tx_done_cb(wifi_tx_done_cb_t cb) { observer_calls++; done_callback=cb; return ESP_OK; }
void test_done(uint8_t id,uint8_t *frame,uint16_t *len,bool ok) { assert(done_callback); done_callback(id,frame,len,ok); }
void tcp_rexmit_rto_commit(struct tcp_pcb *pcb)
{ assert(pcb==expected_pcb); rto_calls++; }
void tcp_rexmit_fast(struct tcp_pcb *pcb)
{
    assert(pcb==expected_pcb); fast_calls++;
    if (!fast_busy && pcb->unacked && !(pcb->flags&TF_INFR)) pcb->flags|=TF_INFR;
}
extern void run_test_calls(void);
void expect_tx(esp_netif_t *n,void *data,size_t len,void *pbuf,int result)
{ expected_netif=n; expected_data=data; expected_len=len; expected_pbuf=pbuf; driver_result=result; }
void expect_pcb(struct tcp_pcb *pcb,bool busy) { expected_pcb=pcb; fast_busy=busy; }
esp_netif_t *get_ap(void) { return &ap; }
esp_netif_t *get_sta(void) { return &sta; }
esp_netif_t *get_other(void) { return &other; }
int main(void)
{
    net_diagnostics_init(&ap,&sta);
    assert(net_diagnostics_start_wifi()==ESP_OK);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    assert(done_callback && observer_calls==1);
#else
    assert(!done_callback && observer_calls==0);
#endif
    run_test_calls();
    assert(driver_calls==8 && rto_calls==3 && fast_calls==5);
    assert(rx_calls==3 && ref_calls==3 && free_calls==3 && held==0);
    assert(rx_frees==3);
    assert(pool_alloc_calls==4 && pool_free_calls==4);
    net_diagnostics_snapshot_t metrics; net_diagnostics_snapshot(&metrics);
    assert(metrics.tcp_pcb_live==0 && metrics.tcp_pcb_peak==2 && metrics.tcp_pcb_failures==1);
    assert(metrics.ap_tx_errors==2 && metrics.sta_tx_errors==2 && metrics.clone_failures==2);
    assert(metrics.client_rto==1 && metrics.upstream_rto==1);
    assert(metrics.client_fast==1 && metrics.upstream_fast==1);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    inject_done_during_clock=true;
    net_diagnostics_log();
    assert(clock_calls>0);
#else
    net_diagnostics_log();
    assert(clock_calls==0);
    puts("basic counters passed; no detailed clock samples or TX observer");
#endif
    puts("wrapper contract passed"); return 0;
}
'''
CALLER = r'''
#include "net_diagnostics.h"
#include "mock_api.h"
#include "proxy_config.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
void expect_tx(esp_netif_t *,void *,size_t,void *,int);
void expect_pcb(struct tcp_pcb *,bool);
esp_netif_t *get_ap(void),*get_sta(void),*get_other(void);
void test_done(uint8_t,uint8_t *,uint16_t *,bool);
void expect_pool(memp_t,void *);
static void tx(esp_netif_t *n,uint8_t *data,size_t len,int result)
{
    uint8_t before[40]; if (data) memcpy(before,data,len);
    int opaque=42; expect_tx(n,data,len,&opaque,result);
    assert(esp_netif_transmit_wrap(n,data,len,&opaque)==result);
    if (data) assert(!memcmp(before,data,len));
    assert(opaque==42);
}
void run_test_calls(void)
{
    int pcb_a=123,pcb_b=456,pool_other=789;
    expect_pool(MEMP_TCP_PCB,&pcb_a); assert(memp_malloc(MEMP_TCP_PCB)==&pcb_a);
    expect_pool(MEMP_TCP_PCB,&pcb_b); assert(memp_malloc(MEMP_TCP_PCB)==&pcb_b);
    expect_pool(MEMP_TCP_PCB,NULL); assert(memp_malloc(MEMP_TCP_PCB)==NULL);
    memp_free(MEMP_TCP_PCB,NULL);
    expect_pool(MEMP_UDP_PCB,&pool_other); assert(memp_malloc(MEMP_UDP_PCB)==&pool_other);
    memp_free(MEMP_UDP_PCB,&pool_other);
    expect_pool(MEMP_TCP_PCB,&pcb_a); memp_free(MEMP_TCP_PCB,&pcb_a);
    expect_pool(MEMP_TCP_PCB,&pcb_b); memp_free(MEMP_TCP_PCB,&pcb_b);
    assert(pcb_a==123 && pcb_b==456 && pool_other==789);
    uint8_t udp[34]={0}; udp[12]=8; udp[14]=0x45; udp[23]=17;
    uint32_t before=net_diagnostics_ap_udp_attempts();
    tx(get_ap(),udp,sizeof(udp),ESP_ERR_NO_MEM);
    assert(net_diagnostics_ap_udp_attempts()==before+1);
    net_diagnostics_udp_mem(before,true); /* Actual UDP driver rejection. */
    before=net_diagnostics_ap_udp_attempts();
    uint8_t arp[34]={0}; arp[12]=8; arp[13]=6;
    tx(get_ap(),arp,sizeof(arp),ESP_OK);
    assert(net_diagnostics_ap_udp_attempts()==before);
    net_diagnostics_udp_mem(before,false); /* ARP send is not UDP send. */
    net_diagnostics_udp_mem(before,true); /* Pre-driver failure with resolved ARP. */
    tx(get_ap(),udp,sizeof(udp),ESP_ERR_ESP_NETIF_TX_FAILED);
    tx(get_sta(),udp,sizeof(udp),ESP_ERR_NO_MEM);
    tx(get_sta(),udp,sizeof(udp),-987); /* Preserve an arbitrary error value. */
    tx(get_other(),udp,sizeof(udp),ESP_ERR_NO_MEM);
    tx(get_ap(),udp,13,ESP_OK); /* Short Ethernet frame, no out of bounds. */
    tx(get_ap(),NULL,0,ESP_OK);
    assert(net_diagnostics_ap_udp_attempts()==before+1);
    struct tcp_pcb client={.local_port=PROXY_ECHO_PORT,.remote_port=40000,.unacked=&udp,.state=ESTABLISHED};
    struct tcp_pcb upstream={.local_port=50000,.remote_port=PROXY_SOCKS5_PORT,.unacked=&udp,.state=SYN_SENT};
    struct tcp_pcb other={.local_port=50001,.remote_port=80,.unacked=&udp};
    struct tcp_pcb *pcbs[]={&client,&upstream,&other};
    for (unsigned i=0;i<3;i++) {
        struct tcp_pcb saved=*pcbs[i]; expect_pcb(pcbs[i],false); tcp_rexmit_rto_commit(pcbs[i]);
        assert(!memcmp(pcbs[i],&saved,sizeof(saved))); /* Diagnostic did not mutate PCB. */
    }
    expect_pcb(&client,false); tcp_rexmit_fast(&client); /* First successful recovery. */
    tcp_rexmit_fast(&client); /* Already recovering, not a second event. */
    expect_pcb(&upstream,true); tcp_rexmit_fast(&upstream); /* Busy, no event. */
    expect_pcb(&upstream,false); tcp_rexmit_fast(&upstream);
    expect_pcb(&other,false); tcp_rexmit_fast(&other); /* Unrelated TCP not included. */
    net_diagnostics_clone_failed(false); net_diagnostics_clone_failed(true);
    esp_netif_t *interfaces[]={get_ap(),get_sta(),get_other()};
    int cookies[3]={123,123,123};
    for (unsigned i=0;i<3;i++) {
        void *buffer=malloc(32); assert(buffer);
        int result=i==1?ESP_ERR_NO_MEM:ESP_OK;
        expect_tx(interfaces[i],buffer,32,&cookies[i],result);
        assert(esp_netif_receive(interfaces[i],buffer,32,&cookies[i])==result);
        assert(cookies[i]==123);
    }
    /* Deferred RX release: the driver owns the original buffer, not the
     * wrapper. Verify exact pointers and two simultaneously held buffers. */
    for (unsigned i=0;i<3;i++) {
        expect_tx(interfaces[i],NULL,0,&cookies[i],ESP_OK);
        esp_netif_free_rx_buffer(interfaces[i],&cookies[i]);
        assert(cookies[i]==123);
    }
    int opaque=987;
    esp_netif_netstack_buf_ref(&opaque); esp_netif_netstack_buf_ref(&opaque);
    esp_netif_netstack_buf_free(&opaque); esp_netif_netstack_buf_free(&opaque);
    esp_netif_netstack_buf_ref(NULL); esp_netif_netstack_buf_free(NULL);
    assert(opaque==987);
#if CONFIG_PROXY_VERBOSE_DIAGNOSTICS
    uint8_t frame[24]={0x88}; frame[4]=0x02;
    uint8_t saved[24]; memcpy(saved,frame,24);
    uint16_t len=24;
    test_done(WIFI_IF_AP,frame,&len,true); test_done(WIFI_IF_AP,frame,&len,false);
    test_done(WIFI_IF_STA,frame,&len,true); test_done(9,frame,&len,false);
    assert(len==24 && !memcmp(frame,saved,24));
    frame[4]=1; test_done(WIFI_IF_AP,frame,&len,true); /* Multicast. */
    frame[4]=2; frame[0]=0x80; test_done(WIFI_IF_AP,frame,&len,true); /* Beacon. */
    frame[0]=0xc8; test_done(WIFI_IF_AP,frame,&len,true); /* QoS Null. */
    frame[0]=0x48; test_done(WIFI_IF_AP,frame,&len,true); /* Null. */
    uint8_t short_frame[1]={0x88}; uint16_t short_len=1;
    test_done(WIFI_IF_AP,short_frame,&short_len,true);
    test_done(WIFI_IF_AP,NULL,&len,true); test_done(WIFI_IF_AP,frame,NULL,true);
#endif
}
'''


class DiagnosticTests(unittest.TestCase):
    @staticmethod
    def run_harness(verbose):
        with tempfile.TemporaryDirectory(prefix='esp-net-diag-test-') as directory:
            path = pathlib.Path(directory)
            (path / 'mock_api.h').write_text(API)
            for header in ('sdkconfig.h', 'esp_netif.h', 'esp_attr.h', 'esp_log.h',
                           'esp_netif_net_stack.h', 'esp_timer.h', 'esp_private/wifi.h',
                           'lwip/priv/tcp_priv.h', 'lwip/memp.h'):
                target = path / header
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('#include "mock_api.h"\n')
            (path / 'harness.c').write_text(HARNESS)
            (path / 'caller.c').write_text(CALLER)
            binary = path / 'check'
            wrappers = ['esp_netif_transmit_wrap', 'tcp_rexmit_rto_commit',
                        'tcp_rexmit_fast', 'memp_malloc', 'memp_free']
            if verbose:
                wrappers += ['esp_netif_receive', 'esp_netif_free_rx_buffer',
                             'esp_netif_netstack_buf_ref', 'esp_netif_netstack_buf_free']
            subprocess.run(['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            f'-DCONFIG_PROXY_VERBOSE_DIAGNOSTICS={int(verbose)}',
                            '-I', str(path), '-I', str(ROOT / 'main'),
                            str(path / 'harness.c'), str(path / 'caller.c'),
                            str(ROOT / 'main/net_diagnostics.c'),
                            *[f'-Wl,--wrap={symbol}' for symbol in wrappers],
                            '-o', str(binary)], check=True)
            result = subprocess.check_output([str(binary)], text=True)
            symbols = subprocess.check_output(['nm', str(binary)], text=True)
        return result, symbols

    def test_default_keeps_basic_counters_without_detailed_wrappers(self):
        result, symbols = self.run_harness(False)
        self.assertIn('basic counters passed; no detailed clock samples or TX observer', result)
        self.assertIn('wrapper contract passed', result)
        self.assertNotIn('driver_rx', result)
        for symbol in ('esp_netif_receive', 'esp_netif_free_rx_buffer',
                       'esp_netif_netstack_buf_ref', 'esp_netif_netstack_buf_free'):
            self.assertNotIn(f'__wrap_{symbol}', symbols)

    def test_verbose_wrapped_calls_preserve_packets_results_and_pcbs(self):
        result, symbols = self.run_harness(True)
        for symbol in ('esp_netif_receive', 'esp_netif_free_rx_buffer',
                       'esp_netif_netstack_buf_ref', 'esp_netif_netstack_buf_free'):
            self.assertIn(f'__wrap_{symbol}', symbols)
        self.assertIn('AP calls=5 mem=1 buf=1 other=0 STA calls=2 mem=1 buf=0 other=1', result)
        self.assertIn('ARP_missing=1 ARP_ready=1 driver_called=1', result)
        self.assertIn('tcp_clone_fail input=1 output=1', result)
        self.assertIn('client RTO=1 fast=1 upstream RTO=1 fast=1', result)
        self.assertIn('client handshake=0 established=1 closing=0 upstream handshake=1 established=0 closing=0', result)
        self.assertIn('wrapper contract passed', result)
        self.assertIn('driver_rx AP calls=1 bytes=32 rejected=0', result)
        self.assertIn('STA calls=1 bytes=32 rejected=1', result)
        self.assertRegex(result, r'wifi_done AP callbacks=9 data_ok=1 data_fail=1 age_ms=\d+ short=3 STA callbacks=2 data_ok=2 data_fail=0')
        ages = [int(value) for value in re.findall(r'age_ms=(\d+)', result)]
        self.assertEqual(len(ages), 4)
        self.assertTrue(all(age < 1000 for age in ages), result)
        self.assertIn('driver_pbuf refs=2 frees=2 held=0', result)
        self.assertIn('RX_buffers AP handed=1 freed=1 held=0 STA handed=1 freed=1 held=0 total=0 peak=2 dynamic_limit=48', result)
        self.assertIn('tcp_pcb live=0 peak=2 limit=256 allocations=3 failures=1', result)


if __name__ == '__main__':
    unittest.main()
