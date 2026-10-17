"""Real sockets exercise the production HTTP deadline and leftover-body guard."""
import pathlib
import subprocess
import tempfile
import unittest

ROOT=pathlib.Path(__file__).resolve().parents[1]
HEADER=r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
typedef void *httpd_handle_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define HTTPD_SOCK_ERR_FAIL -1
#define HTTPD_SOCK_ERR_TIMEOUT -3
typedef struct {int fd;size_t content_len;void *user_ctx;} httpd_req_t;
typedef struct {esp_err_t (*handler)(httpd_req_t *);} httpd_uri_t;
typedef int (*httpd_recv_func_t)(httpd_handle_t,int,char *,size_t,int);
typedef int (*httpd_send_func_t)(httpd_handle_t,int,const char *,size_t,int);
esp_err_t httpd_sess_set_recv_override(httpd_handle_t,int,httpd_recv_func_t);
esp_err_t httpd_sess_set_send_override(httpd_handle_t,int,httpd_send_func_t);
int httpd_req_to_sockfd(httpd_req_t *);
int httpd_req_recv(httpd_req_t *,char *,size_t);
'''
HARNESS=r'''
#include "management_http.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static httpd_recv_func_t readers[64];static httpd_send_func_t writers[64];
static int64_t clock_override;
int64_t esp_timer_get_time(void){if(clock_override)return clock_override;struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000;}
static void delay(unsigned us){struct timespec t={.tv_sec=us/1000000,.tv_nsec=(us%1000000)*1000};nanosleep(&t,NULL);}
esp_err_t httpd_sess_set_recv_override(httpd_handle_t h,int fd,httpd_recv_func_t f){(void)h;assert(fd<64);readers[fd]=f;return ESP_OK;}
esp_err_t httpd_sess_set_send_override(httpd_handle_t h,int fd,httpd_send_func_t f){(void)h;assert(fd<64);writers[fd]=f;return ESP_OK;}
int httpd_req_to_sockfd(httpd_req_t *r){return r->fd;}
int httpd_req_recv(httpd_req_t *r,char *b,size_t n){return readers[r->fd](NULL,r->fd,b,n,0);}
static unsigned called;static char status[64];
#define BODY_MAX 1024
#define API_ERROR(code,message) "error"
static esp_err_t reply(httpd_req_t *r,const char *s,const char *body){(void)r;(void)body;strcpy(status,s);return ESP_OK;}
static esp_err_t untouched(httpd_req_t *r){(void)r;called++;return ESP_OK;}
static esp_err_t consumed(httpd_req_t *r){char bytes[8];called++;return management_http_recv(r,bytes,r->content_len)==(int)r->content_len?ESP_OK:ESP_FAIL;}
/* Exact production route guard is inserted here by the Python runner. */
__GUARD__
int main(int argc,char **argv){
 assert(argc==2);int pair[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair));
 assert(management_http_open(NULL,pair[0])==ESP_OK);
 httpd_uri_t route={.handler=untouched};httpd_req_t req={.fd=pair[0],.user_ctx=&route};char byte;
 if(!strcmp(argv[1],"body")){
  req.content_len=1000000;assert(managed_request(&req)==ESP_FAIL);
  assert(!called&&!strcmp(status,"413 Payload Too Large"));
  req.content_len=16;assert(managed_request(&req)==ESP_FAIL&&called==1);
  /* Returning failure is the SDK's direct-close path, not its purge path. */
  management_http_close(NULL,pair[0]);assert(recv(pair[1],&byte,1,0)==0);pair[0]=-1;
 }else if(!strcmp(argv[1],"read")){
  /* Drip input repeatedly arrives before the individual socket timeout.
   * Header reads and later body reads must share the original deadline.
   * Advance the clock explicitly: host scheduling during a parallel build
   * must not turn an intended 15 ms interval into a deadline expiration. */
  const int64_t start=10000000;
  for(unsigned i=0;i<4;i++){
   clock_override=start+(int64_t)i*15000; /* 0, 15, 30 and 45 ms. */
   assert(write(pair[1],"x",1)==1);assert(readers[pair[0]](NULL,pair[0],&byte,1,0)==1);
  }
  clock_override=start+60000;
  req.content_len=1;management_http_begin(&req);assert(write(pair[1],"x",1)==1);
  assert(management_http_recv(&req,&byte,1)==HTTPD_SOCK_ERR_TIMEOUT);
  assert(management_http_finish(&req,ESP_OK)==ESP_FAIL);
 }else if(!strcmp(argv[1],"write")){
  int small=1024;assert(!setsockopt(pair[0],SOL_SOCKET,SO_SNDBUF,&small,sizeof(small)));
  char data[16384];memset(data,'x',sizeof(data));int n;int64_t start=esp_timer_get_time();
  do{n=writers[pair[0]](NULL,pair[0],data,sizeof(data),0);}while(n>0);
  assert(n==HTTPD_SOCK_ERR_TIMEOUT&&esp_timer_get_time()-start<200000);
 }else if(!strcmp(argv[1],"edge")){
  clock_override=10000000;assert(write(pair[1],"x",1)==1);
  assert(readers[pair[0]](NULL,pair[0],&byte,1,0)==1);
  struct timeval before,after;socklen_t length=sizeof(before);
  assert(!getsockopt(pair[0],SOL_SOCKET,SO_RCVTIMEO,&before,&length));
  clock_override+=49500; /* 500 us left: lwIP rounds this to an infinite wait. */
  assert(readers[pair[0]](NULL,pair[0],&byte,1,0)==HTTPD_SOCK_ERR_TIMEOUT);
  length=sizeof(after);assert(!getsockopt(pair[0],SOL_SOCKET,SO_RCVTIMEO,&after,&length));
  assert(before.tv_sec==after.tv_sec&&before.tv_usec==after.tv_usec);
  assert(writers[pair[0]](NULL,pair[0],"x",1,0)==HTTPD_SOCK_ERR_TIMEOUT);
 }else if(!strcmp(argv[1],"keepalive")){
  route.handler=consumed;
  for(unsigned i=0;i<3;i++){
   req.content_len=2;assert(write(pair[1],"{}",2)==2);assert(managed_request(&req)==ESP_OK);
   delay(60000); /* Idle time belongs to no request, so reuse still works. */
  }
  assert(called==3);
 }else if(!strcmp(argv[1],"slots")){
  int other[2],third[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,other));assert(!socketpair(AF_UNIX,SOCK_STREAM,0,third));
  assert(management_http_open(NULL,other[0])==ESP_OK);
  assert(management_http_open(NULL,third[0])==ESP_FAIL);
  management_http_close(NULL,other[0]);close(other[1]);
  assert(management_http_open(NULL,third[0])==ESP_OK);management_http_close(NULL,third[0]);close(third[1]);
 }else assert(0);
 if(pair[0]>=0)management_http_close(NULL,pair[0]);close(pair[1]);puts("PASS: HTTP guard");
}
'''

class HTTPTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='management-http-');cls.addClassCleanup(cls.temp.cleanup)
        p=pathlib.Path(cls.temp.name);(p/'lwip').mkdir()
        (p/'esp_http_server.h').write_text(HEADER)
        (p/'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
        (p/'lwip/sockets.h').write_text('#include <sys/socket.h>\n#include <sys/time.h>\n')
        source=(ROOT/'main/web_management.c').read_text()
        guard='static esp_err_t managed_request'+source.split('static esp_err_t managed_request',1)[1].split('static esp_err_t missing_route',1)[0]
        (p/'check.c').write_text(HARNESS.replace('__GUARD__',guard));cls.binary=p/'check'
        subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DMANAGEMENT_HTTP_REQUEST_US=50000',
            '-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I',str(p),'-I',str(ROOT/'main'),
            str(p/'check.c'),str(ROOT/'main/management_http.c'),'-o',str(cls.binary)],check=True)
    def check(self,mode):subprocess.run([str(self.binary),mode],check=True,timeout=3)
    def test_rejected_unread_body_closes_without_sdk_purge(self):self.check('body')
    def test_slow_headers_and_body_share_total_deadline(self):self.check('read')
    def test_slow_response_receiver_has_total_deadline(self):self.check('write')
    def test_sub_millisecond_budget_cannot_disable_lwip_timeout(self):self.check('edge')
    def test_complete_requests_can_reuse_idle_keepalive(self):self.check('keepalive')
    def test_session_storage_is_bounded_and_reusable(self):self.check('slots')
    def test_json_media_type_accepts_parameters_without_weakening_action_marker(self):
        source=(ROOT/'main/web_management.c').read_text()
        functions='static bool json_content_type'+source.split('static bool json_content_type',1)[1].split('static bool authorize',1)[0]
        code=r'''
#include <stdbool.h>
#include <stddef.h>
#include <assert.h>
#include <string.h>
#define ESP_OK 0
#define API_ERROR(code,message) code
typedef struct {const char *marker,*type,*origin;} httpd_req_t;
static bool rebooting;
static const char origin[]="http://192.168.4.1";
static const char *header(httpd_req_t *r,const char *name) {
 return !strcmp(name,"X-Proxy-Action")?r->marker:!strcmp(name,"Content-Type")?r->type:r->origin;
}
static int httpd_req_get_hdr_value_str(httpd_req_t *r,const char *name,char *out,size_t capacity) {
 const char *value=header(r,name);if(!value || strlen(value)>=capacity)return -1;strcpy(out,value);return ESP_OK;
}
static size_t httpd_req_get_hdr_value_len(httpd_req_t *r,const char *name) {
 const char *value=header(r,name);return value?strlen(value):0;
}
static int reply(httpd_req_t *r,const char *status,const char *body) {(void)r;(void)status;(void)body;return ESP_OK;}
'''+functions+r'''
int main(void) {
 const char *valid[]={"application/json","application/json; charset=utf-8","Application/JSON;charset=UTF-8"," application/json \t; charset=utf-8"};
 const char *invalid[]={"","text/plain","application/jsonp","application/json junk","application/jsonx; charset=utf-8","application/jso"};
 for(size_t i=0;i<sizeof(valid)/sizeof(*valid);i++)assert(json_content_type(valid[i]));
 for(size_t i=0;i<sizeof(invalid)/sizeof(*invalid);i++)assert(!json_content_type(invalid[i]));
 httpd_req_t r={.marker="manage",.type="application/json; charset=utf-8",.origin=origin};
 assert(mutation_allowed(&r));
 r.marker=NULL;assert(!mutation_allowed(&r));r.marker="manage";
 r.origin="http://example.com";assert(!mutation_allowed(&r));
 r.origin=NULL;assert(mutation_allowed(&r)); /* Command-line clients may omit Origin. */
 rebooting=true;assert(!mutation_allowed(&r));
}
'''
        p=pathlib.Path(self.temp.name);(p/'media-type.c').write_text(code)
        subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror',str(p/'media-type.c'),'-o',str(p/'media-type')],check=True)
        subprocess.run([str(p/'media-type')],check=True)
