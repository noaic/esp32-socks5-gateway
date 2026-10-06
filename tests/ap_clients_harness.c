#include "ap_clients.h"
#include "flow_table.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    const uint8_t a[6]={2,0,0,0,0,1},b[6]={2,0,0,0,0,2},c[6]={2,0,0,0,0,3};
    uint32_t ta=proxy_client_connect(a); assert(ta);
    assert(!proxy_client_current(ta)); assert(proxy_client_assign(a,1));
    assert(proxy_client_current(ta)); assert(proxy_client_token(1)==ta);
    assert(proxy_client_assign(a,1)); assert(proxy_client_connect(a)==ta);
    for (unsigned i=0;i<64;i++) assert(proxy_client_acquire(ta,false,false));
    uint32_t tb=proxy_client_connect(b); assert(tb && tb!=ta);
    assert(!proxy_client_assign(b,1)); assert(proxy_client_assign(b,2));
    assert(!proxy_client_connect(c)); assert(!proxy_client_acquire(ta,false,false));
    for (unsigned i=0;i<16;i++) assert(proxy_client_acquire(tb,false,false));
    for (unsigned i=0;i<64;i++) proxy_client_release(ta,false,false);
    for (unsigned i=0;i<48;i++) assert(proxy_client_acquire(ta,false,false));
    assert(!proxy_client_acquire(ta,false,false));
    for (unsigned i=0;i<22;i++) assert(proxy_client_acquire(ta,true,false));
    assert(!proxy_client_acquire(ta,true,false));
    assert(proxy_client_acquire(ta,true,true)); assert(proxy_client_acquire(ta,true,true));
    assert(!proxy_client_acquire(ta,true,true));
    proxy_client_traffic(tb,false,false,1234);
    proxy_client_disconnect(a); assert(!proxy_client_current(ta)); assert(proxy_client_current(tb));
    uint32_t anew=proxy_client_connect(a); assert(anew!=ta); assert(proxy_client_assign(a,1));
    proxy_client_release(ta,false,false); proxy_client_traffic(ta,false,true,999);
    proxy_client_info_t info[2]; proxy_clients_snapshot(info);
    assert(info[anew&1].tcp==0 && info[anew&1].tcp_up==0);
    assert(info[tb&1].tcp==16 && info[tb&1].tcp_down==1234);
    assert(proxy_client_assign(a,3)); assert(!proxy_client_current(anew));
    assert(proxy_client_current(proxy_client_token(3)));
    static flow_table_t table; flow_table_init(&table); flow_t snap; bool created;
    proxy_tcp_packet_t p={.src_ip=1,.src_port=50000,.dst_ip=9,.dst_port=443,.flags=2};
    int old=flow_table_input_owned(&table,&p,ta,1,&snap,&created); assert(old>=0 && created);
    int fresh=flow_table_input_owned(&table,&p,anew,2,&snap,&created); assert(fresh>=0 && fresh!=old && created);
    p.src_ip=2;
    int other=flow_table_input_owned(&table,&p,tb,3,&snap,&created); assert(other>=0 && other!=fresh);
    assert(flow_table_peer(&table,2,snap.local_peer_port)==other);
    assert(flow_table_peer(&table,1,snap.local_peer_port)==-1);
    proxy_clients_reset(); assert(!proxy_client_current(tb)); assert(proxy_client_connect(a)!=anew);
    puts("client lifecycle, quotas, stale release, and tuple isolation passed");
    return 0;
}
