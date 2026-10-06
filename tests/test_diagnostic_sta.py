"""Actual diagnostic STA client against a controlled loopback SOCKS/HTTP peer."""
import hashlib
import json
import socket
import subprocess
import threading
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'tests/.out/diagnostic_sta'
OUT.mkdir(parents=True, exist_ok=True)
for name, body in {
 'sdkconfig.h': '#define CONFIG_PROXY_LINK_BENCH 1\n',
 'esp_http_server.h': '#pragma once\n#include <stdbool.h>\n#include <stddef.h>\ntypedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_FAIL -1\ntypedef struct {int unused;} httpd_req_t;\nint httpd_resp_set_type(httpd_req_t *,const char *);\nint httpd_resp_sendstr(httpd_req_t *,const char *);\n',
 'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time(void);\n',
 'esp_log.h': '#pragma once\n#define ESP_LOGI(...) ((void)0)\n',
 'lwip/sockets.h': '#pragma once\n#include <sys/socket.h>\n#include <sys/select.h>\n#include <netinet/tcp.h>\n#include <unistd.h>\n#define lwip_select select\n',
 'lwip/inet.h': '#pragma once\n#include <arpa/inet.h>\n',
}.items():
 p=OUT/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(body)
harness=OUT/'harness.c'
harness.write_text(r'''#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_http_server.h"
#include "proxy_config.h"
#undef PROXY_SOCKS5_IPV4
#undef PROXY_SOCKS5_PORT
static int test_port;
#define PROXY_SOCKS5_IPV4 "127.0.0.1"
#define PROXY_SOCKS5_PORT test_port
int64_t esp_timer_get_time(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
int httpd_resp_set_type(httpd_req_t *r,const char *t) {(void)r;(void)t;return 0;}
int httpd_resp_sendstr(httpd_req_t *r,const char *s) {(void)r;puts(s);return 0;}
#include "diagnostic_sta.c"
int main(int argc,char **argv) {
 if(argc!=2)return 2;test_port=atoi(argv[1]);
 unsigned char pattern[16384],buffer[8192];for(unsigned i=0;i<sizeof(pattern);i++)pattern[i]=i&255;
 httpd_req_t req={0};return diagnostic_sta_run(&req,pattern,buffer,sizeof(pattern),sizeof(buffer))!=ESP_OK;
}
''')
cjson=ROOT/'managed_components/espressif__cjson/cJSON'
subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation',
 '-I',str(OUT),'-I',str(ROOT/'main'),'-I',str(cjson),str(harness),str(cjson/'cJSON.c'),'-lm','-o',str(OUT/'client')],check=True)
BLOCK=bytes(range(256))*128
TOTAL=8*1024*1024
DIGEST='7d212b9c884f5c77896de960ae17cc341cda43b14d6a971f34ca29ebd4badf7f'

def exact(sock,n):
 data=b''
 while len(data)<n:
  chunk=sock.recv(n-len(data))
  if not chunk:raise EOFError()
  data+=chunk
 return data

class Peer:
 def __init__(self,mode):
  self.mode=mode;self.uploads=0;self.errors=[];self.stop=threading.Event()
  self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen();self.listener.settimeout(.1)
  self.port=self.listener.getsockname()[1]
  self.thread=threading.Thread(target=self.serve);self.thread.start()
 def close(self):
  self.stop.set();self.thread.join(5);self.listener.close()
  if self.thread.is_alive():raise AssertionError('peer thread did not stop')
 def serve(self):
  while not self.stop.is_set():
   try:s,_=self.listener.accept()
   except socket.timeout:continue
   with s:
    s.settimeout(3)
    try:self.handle(s)
    except (OSError,EOFError):pass # Client is expected to close rejected transfers.
    except AssertionError as e:self.errors.append(str(e))
 def handle(self,s):
  assert exact(s,3)==bytes([5,1,0]),'invalid NO AUTH greeting'
  if self.mode=='noauth':s.sendall(bytes([5,2]));return
  s.sendall(bytes([5]));s.sendall(bytes([0])) # exercise short handshake replies
  assert exact(s,10)==bytes([5,1,0,1,192,168,1,10,70,160]),'invalid IPv4 CONNECT destination'
  reply=bytes([5,5 if self.mode=='reject' else 0,0,1,127,0,0,1,1,1])
  s.sendall(reply[:3]);s.sendall(reply[3:])
  if self.mode=='reject':return
  header=b''
  while not header.endswith(b'\r\n\r\n'):
   header+=exact(s,1)
   assert len(header)<512,'unbounded HTTP request header'
  if header.startswith(b'GET /8m '):
   if self.mode=='chunked':s.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 8388608\r\nTransfer-Encoding: chunked\r\n\r\n');return
   if self.mode=='oversized_header':s.sendall(b'HTTP/1.1 200 OK\r\nX: '+b'x'*1100+b'\r\n\r\n');return
   length=TOTAL-1 if self.mode=='wrong_length' else TOTAL
   s.sendall(f'HTTP/1.1 200 OK\r\nContent-Length: {length}\r\nConnection: close\r\n\r\n'.encode())
   if self.mode=='wrong_length':return
   if self.mode=='truncated':s.sendall(BLOCK);return
   first=bytearray(BLOCK)
   if self.mode=='corrupt':first[0]^=1
   s.sendall(first[:7]);s.sendall(first[7:])
   for _ in range(TOTAL//len(BLOCK)-1):s.sendall(BLOCK)
  elif header.startswith(b'PUT /upload '):
   assert b'Content-Length: 8388608\r\n' in header,'wrong upload length'
   digest=hashlib.sha256();remaining=TOTAL
   while remaining:
    part=s.recv(min(32768,remaining))
    if not part:raise EOFError()
    digest.update(part);remaining-=len(part)
   assert digest.hexdigest()==DIGEST,'incorrect upload bytes'
   self.uploads+=1
   if self.mode=='oversized_response':s.sendall(b'HTTP/1.1 200 OK\r\nContent-Length: 8192\r\n\r\n');return
   body=json.dumps({'bytes':TOTAL,'matches_8m':self.mode!='bad_upload'}).encode()
   s.sendall(f'HTTP/1.1 200 OK\r\nContent-Length: {len(body)}\r\n\r\n'.encode()+body)
  else:raise AssertionError('unexpected HTTP method')

class DiagnosticTests(unittest.TestCase):
 def run_mode(self,mode):
  peer=Peer(mode)
  try:
   result=subprocess.run([str(OUT/'client'),str(peer.port)],capture_output=True,text=True,timeout=20,check=True)
   report=json.loads(result.stdout);self.assertEqual(peer.errors,[])
   return report,peer.uploads
  finally:peer.close()
 def test_complete_content_and_three_rounds(self):
  report,uploads=self.run_mode('ok');self.assertEqual(report['phase'],'complete');self.assertEqual(len(report['rounds']),3);self.assertEqual(uploads,3)
  for r in report['rounds']:self.assertGreater(r['down'],0);self.assertGreater(r['up'],0)
 def test_rejected_or_invalid_transfers_never_report_complete(self):
  for mode in ('noauth','reject','chunked','oversized_header','wrong_length','truncated','corrupt','bad_upload','oversized_response'):
   with self.subTest(mode=mode):
    report,_=self.run_mode(mode);self.assertEqual(report['phase'],'error');self.assertEqual(report['rounds'],[])

if __name__=='__main__':unittest.main()
