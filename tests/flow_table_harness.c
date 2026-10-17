#include "flow_table.h"
#include <assert.h>
#include <stdio.h>
static flow_table_t table;
int main(void)
{
    flow_table_init(&table);
    proxy_tcp_packet_t p={.src_ip=0x0204a8c0,.src_port=50000,.dst_port=443,.flags=2};
    flow_t snapshot; bool created;
    for (unsigned i=0;i<PROXY_MAX_TCP_MAPPINGS;i++) {
        p.dst_ip=0x01020300+i;
        int index=flow_table_input(&table,&p,10,&snapshot,&created);
        assert(index==(int)i && created);
        assert(snapshot.local_peer_port==PROXY_FLOW_PORT_BASE+i);
        assert(flow_table_peer(&table,p.src_ip,snapshot.local_peer_port)==index);
        assert(flow_table_peer(&table,p.src_ip+1,snapshot.local_peer_port)==-1);
    }
    assert(table.used==PROXY_MAX_TCP_MAPPINGS);
    p.dst_ip=0xffffffff;
    assert(flow_table_input(&table,&p,10,&snapshot,&created)==-1);
    // Every original full tuple still resolves at full capacity, same client port.
    p.flags=16;
    for (unsigned i=0;i<PROXY_MAX_TCP_MAPPINGS;i++) {
        p.dst_ip=0x01020300+i;
        assert(flow_table_input(&table,&p,111,&snapshot,&created)==(int)i && !created);
        assert(snapshot.input_packets==2);
    }
    assert(flow_table_expire(&table,210,100)==0);
    table.flows[10].socket_open=true;
    table.flows[20].retired=true; table.flows[20].retired_us=210;
    assert(flow_table_expire(&table,211,100)==PROXY_MAX_TCP_MAPPINGS-2);
    assert(table.used==2);
    assert(flow_table_peer(&table,p.src_ip,PROXY_FLOW_PORT_BASE)==-1);
    assert(flow_table_expire(&table,309,100)==0);
    assert(flow_table_expire(&table,310,100)==1);
    table.flows[10].socket_open=false;
    assert(flow_table_expire(&table,310,100)==1 && table.used==0);
    // Hash unlink and slot reuse after guards, no allocation on unknown ACK.
    assert(flow_table_input(&table,&p,400,&snapshot,&created)==-1);
    p.flags=2;
    assert(flow_table_input(&table,&p,400,&snapshot,&created)>=0 && created);
    assert(flow_table_input(&table,&p,401,&snapshot,&created)>=0 && !created);
    assert(table.used==1);
    /* An unacknowledged SYN burst must not consume the other client's
     * metadata or hundreds of lwIP half-open PCBs. Retransmits still resolve. */
    flow_table_init(&table);p.flags=2;p.src_ip=1;
    for(unsigned i=0;i<PROXY_CLIENT_TCP_PENDING;i++){
        p.dst_ip=1000+i;assert(flow_table_input_owned(&table,&p,2,1,&snapshot,&created)>=0);
    }
    p.dst_ip=2000;assert(flow_table_input_owned(&table,&p,2,1,&snapshot,&created)==-1);
    p.dst_ip=1000;assert(flow_table_input_owned(&table,&p,2,2,&snapshot,&created)>=0&&!created);
    p.src_ip=2;p.dst_ip=2000;int b=flow_table_input_owned(&table,&p,3,2,&snapshot,&created);assert(b>=0);
    assert(flow_table_claim(&table,b,&snapshot));assert(!flow_table_claim(&table,b,&snapshot));
    assert(table.pending[1]==0);flow_table_retire(&table,b,2);assert(table.pending[1]==0);
    assert(flow_table_expire(&table,100,100)==0);
    assert(flow_table_expire(&table,102,100)==PROXY_CLIENT_TCP_PENDING+1);
    assert(table.owned[0]==0 && table.pending[0]==0 && table.owned[1]==0);
    /* Retained records count across reconnect tokens of one slot, while
     * the other slot always has its own 256-record capacity. */
    flow_table_init(&table);p.src_ip=1;
    for(unsigned i=0;i<PROXY_CLIENT_TCP_MAPPINGS;i++){
        p.dst_ip=1000+i;int index=flow_table_input_owned(&table,&p,2,1,&snapshot,&created);assert(index>=0);
        assert(flow_table_claim(&table,index,&snapshot));flow_table_retire(&table,index,2);
    }
    p.dst_ip=2000;assert(flow_table_input_owned(&table,&p,4,3,&snapshot,&created)==-1);
    p.src_ip=2;
    for(unsigned i=0;i<PROXY_CLIENT_TCP_MAPPINGS;i++){
        p.dst_ip=2000+i;int index=flow_table_input_owned(&table,&p,3,3,&snapshot,&created);assert(index>=0);
        assert(flow_table_claim(&table,index,&snapshot));flow_table_retire(&table,index,3);
    }
    assert(table.used==512 && table.pending[0]==0 && table.pending[1]==0);
    assert(flow_table_expire(&table,101,100)==0);
    assert(flow_table_expire(&table,102,100)==256 && table.owned[0]==0 && table.owned[1]==256);
    assert(flow_table_expire(&table,103,100)==256 && table.used==0);
    /* Simulated time, not a radio benchmark: a sustained stream of distinct
     * short connections must preserve the other slot and recover after 2MSL. */
    flow_table_init(&table);
    p=(proxy_tcp_packet_t){.src_ip=1,.dst_ip=2,.dst_port=443,.flags=2};
    const int64_t guard=120000000;
    unsigned accepted=0,rejected=0;
    for(unsigned i=0;i<400;i++) {
        int64_t now=(int64_t)i*250000;
        flow_table_expire(&table,now,guard);
        p.src_port=1000+i;
        int index=flow_table_input_owned(&table,&p,2,now,&snapshot,&created);
        if(index<0){rejected++;continue;}
        accepted++;assert(created && flow_table_claim(&table,index,&snapshot));
        flow_table_retire(&table,index,now);
    }
    assert(accepted==PROXY_CLIENT_TCP_MAPPINGS && rejected==400-accepted);
    p.src_ip=2;p.src_port=60000;
    int other=flow_table_input_owned(&table,&p,3,100000000,&snapshot,&created);
    assert(other>=0 && created && flow_table_claim(&table,other,&snapshot));
    flow_table_retire(&table,other,100000000);
    assert(flow_table_expire(&table,guard-1,guard)==0);
    assert(flow_table_expire(&table,guard,guard)==1);
    p.src_ip=1;p.src_port=65000;
    assert(flow_table_input_owned(&table,&p,2,guard,&snapshot,&created)>=0 && created);
    printf("PASS: simulated 400 short connections/100s: accepted=%u rejected=%u; other slot and guarded recovery preserved\n",accepted,rejected);
    puts("PASS: shared source-port isolation, full capacity, guards, hash unlink/reuse");
}
