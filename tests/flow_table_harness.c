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
    puts("PASS: shared source-port isolation, full capacity, guards, hash unlink/reuse");
}
