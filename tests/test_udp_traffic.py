"""Check byte totals using the production UDP accounting statements."""
import pathlib,re,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class TrafficTests(unittest.TestCase):
 def test_payload_bytes_not_packet_count_and_stale_client(self):
  source=(ROOT/'main/udp_gateway.c').read_text()
  upload=re.search(r'static void client_traffic\(.*?\n\{[^\n]+\}',source).group(0)
  down=re.search(r'proxy_client_traffic\(m->client_token,true,false,[^;]+;',source).group(0)
  harness=r'''
#include "ap_clients.h"
#include "udp_relay.h"
#include <assert.h>
__UPLOAD__
static void delivered(uint32_t token,size_t length){udp_datagram_t message={.client_token=token,.tuple.payload_length=length};const udp_datagram_t *m=&message;__DOWN__}
int main(void){
 const uint8_t mac[6]={2,0,0,0,0,1};uint32_t token=proxy_client_connect(mac);assert(proxy_client_assign(mac,1));
 const size_t lengths[]={0,1,1200,1462};for(unsigned i=0;i<4;i++){client_traffic(NULL,token,true,lengths[i]);delivered(token,lengths[i]);}
 proxy_client_info_t clients[2];proxy_clients_snapshot(clients);assert(clients[token&1].udp_up==2663&&clients[token&1].udp_down==2663);
 proxy_client_disconnect(mac);uint32_t fresh=proxy_client_connect(mac);assert(proxy_client_assign(mac,1));
 client_traffic(NULL,token,true,1462);delivered(token,1462);proxy_clients_snapshot(clients);
 assert(fresh!=token&&!clients[fresh&1].udp_up&&!clients[fresh&1].udp_down);return 0;
}
'''.replace('__UPLOAD__',upload).replace('__DOWN__',down)
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'check.c').write_text(harness)
   subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-pthread','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/ap_clients.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
