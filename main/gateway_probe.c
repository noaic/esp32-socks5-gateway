#include "gateway_probe.h"
#include "socks_udp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#endif
static int64_t milliseconds(void) {
#ifdef ESP_PLATFORM
    return esp_timer_get_time()/1000;
#else
    struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
#endif
}
static int wait_fd(int fd,bool write,int64_t deadline,void (*idle)(void))
{
    for (;;) {
        if(idle)idle();
        int64_t left=deadline-milliseconds();if(left<=0)return -ETIMEDOUT;
        if(left>200)left=200;
        fd_set f;FD_ZERO(&f);FD_SET(fd,&f);struct timeval tv={.tv_sec=left/1000,.tv_usec=(left%1000)*1000};
        int n=select(fd+1,write?NULL:&f,write?&f:NULL,NULL,&tv);
        if(n>0)return 0;
        if(n==0)continue;
        if(errno!=EINTR)return -errno;
    }
}
static int exact(int fd,uint8_t *data,size_t length,bool write,int64_t deadline,void (*idle)(void))
{
    size_t offset=0;
    while(offset<length) {
        int err=wait_fd(fd,write,deadline,idle);if(err)return err;
        int n=write?send(fd,data+offset,length-offset,0):recv(fd,data+offset,length-offset,0);
        if(n>0)offset+=(size_t)n;else if(!n)return -ECONNRESET;
        else if(errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)return -errno;
    }
    return 0;
}
static int nonblock(int fd) { int flags=fcntl(fd,F_GETFL,0);return flags<0||fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0?-errno:0; }
static bool public_ip(uint32_t address)
{
    uint32_t ip=ntohl(address);unsigned a=ip>>24,b=(ip>>16)&255;
    return a && a!=10 && a!=127 && a<224 && !(a==172 && b>=16 && b<=31) &&
        !(a==192 && b==168) && !(a==169 && b==254) && !(a==100 && b>=64 && b<=127) &&
        !(a==198 && (b==18 || b==19)) &&
        !((ip&0xffffff00u)==0xc0000200u || (ip&0xffffff00u)==0xc6336400u || (ip&0xffffff00u)==0xcb007100u);
}
static uint16_t u16(const uint8_t *p) {return (uint16_t)((unsigned)p[0]*256+p[1]);}
static bool dns_name(const uint8_t *p,size_t size,size_t *offset,char out[254])
{
    size_t pos=*offset,used=0;bool jumped=false;
    for(unsigned steps=0;steps<128;steps++) {
        if(pos>=size)return false;
        unsigned n=p[pos++];
        if(!n){if(!jumped)*offset=pos;out[used]=0;return true;}
        if((n&0xc0)==0xc0) {
            if(pos>=size)return false;
            unsigned next=((n&63)<<8)|p[pos++];if(!jumped)*offset=pos;
            if(next>=size)return false;
            pos=next;jumped=true;continue;
        }
        if(n>63 || pos+n>size || used+n+(used!=0)>=254)return false;
        if(used)out[used++]='.';
        for(unsigned i=0;i<n;i++){unsigned c=p[pos++];if(!c || c=='.')return false;out[used++]=(char)tolower(c);}
        if(!jumped)*offset=pos;
    }
    return false;
}
static bool dns_ipv4(const uint8_t *p,size_t size,const char *host,uint32_t *ip)
{
    if(size<12 || u16(p+4)!=1 || !u16(p+6) || u16(p+6)>32)return false;
    size_t answers=12;char current[254],name[254],alias[254];
    if(!dns_name(p,size,&answers,current) || strcmp(current,host) || answers+4>size ||
        u16(p+answers)!=1 || u16(p+answers+2)!=1)return false;
    answers+=4;
    for(unsigned chain=0;chain<8;chain++) {
        size_t offset=answers;alias[0]=0;
        for(unsigned i=0;i<u16(p+6);i++) {
            if(!dns_name(p,size,&offset,name) || offset+10>size)return false;
            unsigned type=u16(p+offset),class_=u16(p+offset+2),length=u16(p+offset+8);offset+=10;
            if(offset+length>size)return false;
            if(class_==1 && !strcmp(current,name)) {
                if(type==1 && length==4){uint32_t candidate;memcpy(&candidate,p+offset,4);if(public_ip(candidate)){*ip=candidate;return true;}}
                if(type==5 && !alias[0]){size_t a=offset;if(!dns_name(p,size,&a,alias) || a!=offset+length)return false;}
            }
            offset+=length;
        }
        if(!alias[0] || !strcmp(alias,current))return false;
        strcpy(current,alias);
    }
    return false;
}
static size_t dns_query(uint8_t *query,size_t capacity,const char *host,uint16_t id)
{
    if(capacity<18)return 0;
    memset(query,0,12);query[0]=id>>8;query[1]=id;query[2]=1;query[5]=1;
    size_t used=12;const char *label=host;
    while(*label){const char *end=strchr(label,'.');size_t n=end?(size_t)(end-label):strlen(label);
        if(!n || n>63 || used+1+n+5>capacity)return 0;
        query[used++]=(uint8_t)n;memcpy(query+used,label,n);used+=n;if(!end)break;label=end+1;
    }
    query[used++]=0;query[used++]=0;query[used++]=1;query[used++]=0;query[used++]=1;return used;
}
static void probe_dns(const char *upstream,uint16_t port,const char *dns,const char *host,
                      uint32_t *resolved,gateway_probe_result_t *r,void (*idle)(void))
{
    *r=(gateway_probe_result_t){.code={5000,5000,5000}};
    int tcp=-1,udp=-1,stage=0,err=0;int64_t start=milliseconds(),deadline=start+3000;
    struct sockaddr_in server={.sin_family=AF_INET,.sin_port=htons(port)},destination={.sin_family=AF_INET,.sin_port=htons(53)};
    if(inet_pton(AF_INET,upstream,&server.sin_addr)!=1 || inet_pton(AF_INET,dns,&destination.sin_addr)!=1){err=-EINVAL;goto done;}
    tcp=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(tcp<0){err=-errno;goto done;}
    if((err=nonblock(tcp)))goto done;
    if(connect(tcp,(struct sockaddr *)&server,sizeof(server))) {
        if(errno!=EINPROGRESS){err=-errno;goto done;}
        if((err=wait_fd(tcp,true,deadline,idle)))goto done;
        int e=0;socklen_t n=sizeof(e);if(getsockopt(tcp,SOL_SOCKET,SO_ERROR,&e,&n)){err=-errno;goto done;}
        if(e){err=-e;goto done;}
    }
    r->code[0]=0;r->elapsed_ms[0]=(uint32_t)(milliseconds()-start);
    stage=1;start=milliseconds();deadline=start+2000;
    uint8_t hello[]={5,1,0},response[10];
    if((err=exact(tcp,hello,sizeof(hello),true,deadline,idle)) || (err=exact(tcp,response,2,false,deadline,idle)))goto done;
    if(response[0]!=5){err=3000;goto done;}if(response[1]){err=2000+response[1];goto done;}
    r->code[1]=0;r->elapsed_ms[1]=(uint32_t)(milliseconds()-start);
    stage=2;start=milliseconds();deadline=start+4000;
    udp=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);if(udp<0){err=-errno;goto done;}
    if((err=nonblock(udp)))goto done;
    struct sockaddr_in local={.sin_family=AF_INET};
    if(bind(udp,(struct sockaddr *)&local,sizeof(local))){err=-errno;goto done;}
    socklen_t len=sizeof(local);if(getsockname(udp,(struct sockaddr *)&local,&len)){err=-errno;goto done;}
    uint8_t associate[]={5,3,0,1,0,0,0,0,0,0};memcpy(associate+8,&local.sin_port,2);
    if((err=exact(tcp,associate,10,true,deadline,idle)) || (err=exact(tcp,response,4,false,deadline,idle)))goto done;
    if(response[0]!=5 || response[2] || response[3]!=1){err=3000;goto done;}if(response[1]){err=1000+response[1];goto done;}
    if((err=exact(tcp,response+4,6,false,deadline,idle)))goto done;
    struct sockaddr_in relay={.sin_family=AF_INET};memcpy(&relay.sin_addr,response+4,4);memcpy(&relay.sin_port,response+8,2);
    if(!relay.sin_addr.s_addr)relay.sin_addr=server.sin_addr;
    uint32_t rip=ntohl(relay.sin_addr.s_addr);
    if(!relay.sin_port || !(rip>>24) || (rip>>24)>=224){err=3000;goto done;}
#ifdef ESP_PLATFORM
    if((rip>>24)==127){err=3000;goto done;}
#endif
    if(connect(udp,(struct sockaddr *)&relay,sizeof(relay))){err=-errno;goto done;}
    /* One explicit public A query, no private destinations or direct fallback. */
    uint16_t id=(uint16_t)milliseconds();
    uint8_t query[256];size_t query_size=dns_query(query,sizeof(query),host,id);
    if(!query_size){err=-EINVAL;goto done;}
    uint8_t wire[512];size_t bytes=socks_udp_encode(wire,sizeof(wire),destination.sin_addr.s_addr,53,query,query_size);
    if((err=wait_fd(udp,true,deadline,idle)))goto done;
    int sent=send(udp,wire,bytes,0);if(sent!=(int)bytes){err=sent<0?-errno:-EIO;goto done;}
    for (;;) {
        if((err=wait_fd(udp,false,deadline,idle)))goto done;
        int n=recv(udp,wire,sizeof(wire),0);if(n<0){if(errno==EAGAIN||errno==EINTR)continue;err=-errno;goto done;}
        uint32_t ip;uint16_t p;const uint8_t *payload;size_t size;
        if(!socks_udp_decode(wire,(size_t)n,&ip,&p,&payload,&size) || ip!=destination.sin_addr.s_addr || p!=53 || size<12 || payload[0]!=query[0] || payload[1]!=query[1])continue;
        if(!(payload[2]&0x80) || (payload[2]&0x7a) || payload[4]!=0 || payload[5]!=1 || size<query_size || memcmp(payload+12,query+12,query_size-12)) {err=3000;goto done;}
        if(payload[3]&15){err=4000+(payload[3]&15);goto done;}
        if(payload[6]==0 && payload[7]==0){err=3000;goto done;}
        if(resolved && !dns_ipv4(payload,size,host,resolved)){err=3000;goto done;}
        err=0;break;
    }
done:
    r->code[stage]=err;r->elapsed_ms[stage]=(uint32_t)(milliseconds()-start);
    if(udp>=0)close(udp);
    if(tcp>=0)close(tcp);
}
void gateway_probe_run(const char *upstream,uint16_t port,const char *dns,gateway_probe_result_t *r,void (*idle)(void))
{probe_dns(upstream,port,dns,"example.com",NULL,r,idle);}

static const struct {const char *host,*path,*body;unsigned status;} targets[]={
    {"www.msftconnecttest.com","/connecttest.txt","Microsoft Connect Test",200},
    {"connectivitycheck.gstatic.com","/generate_204","",204}
};
const char *gateway_internet_target_url(unsigned target)
{
    static const char *const urls[]={"http://www.msftconnecttest.com/connecttest.txt","http://connectivitycheck.gstatic.com/generate_204"};
    return target<GATEWAY_INTERNET_TARGETS?urls[target]:"";
}
static bool header_is(const char *p,size_t length,const char *name)
{if(strlen(name)!=length)return false;for(size_t i=0;i<length;i++)if(tolower((unsigned char)p[i])!=name[i])return false;return true;}
static int http_response(int fd,unsigned target,int64_t deadline,uint16_t *status,void (*idle)(void))
{
    char wire[1024];size_t used=0,body=0;bool headers=false,has_length=false;size_t length=0;
    size_t expected=strlen(targets[target].body);
    for(;;) {
        if(headers) {
            size_t have=used-body;
            if(have>expected || memcmp(wire+body,targets[target].body,have))return 7000;
            if(has_length && have==expected)return 0;
            if(targets[target].status==204)return have?7000:0;
        }
        if(used>=sizeof(wire)-1)return 3000;
        int err=wait_fd(fd,false,deadline,idle);if(err)return err;
        int n=recv(fd,wire+used,sizeof(wire)-1-used,0);
        if(n<0){if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)continue;return -errno;}
        if(!n)return headers && used-body==expected?0:-ECONNRESET;
        used+=(size_t)n;wire[used]=0;
        if(!headers) {
            char *end=strstr(wire,"\r\n\r\n");if(!end)continue;
            if(used<12 || (memcmp(wire,"HTTP/1.0 ",9) && memcmp(wire,"HTTP/1.1 ",9)) ||
                wire[9]<'1'||wire[9]>'5'||!isdigit((unsigned char)wire[10])||!isdigit((unsigned char)wire[11]) ||
                (wire[12]!=' ' && wire[12]!='\r'))return 3000;
            *status=(uint16_t)((wire[9]-'0')*100+(wire[10]-'0')*10+wire[11]-'0');
            if(*status!=targets[target].status)return 6000+*status;
            char *line=strstr(wire,"\r\n");if(!line || line>end)return 3000;line+=2;
            while(line<end){char *eol=strstr(line,"\r\n"),*colon=strchr(line,':');
                if(!eol||eol>end||!colon||colon>=eol)return 3000;
                if(header_is(line,(size_t)(colon-line),"transfer-encoding"))return 3000;
                if(header_is(line,(size_t)(colon-line),"content-length")) {
                    char *p=colon+1;while(p<eol&&(*p==' '||*p=='\t'))p++;
                    if(p==eol||!isdigit((unsigned char)*p))return 3000;
                    size_t value=0;while(p<eol&&isdigit((unsigned char)*p)){if(value>1024)return 7000;value=value*10+(*p++-'0');}
                    while(p<eol&&(*p==' '||*p=='\t'))p++;
                    if(p!=eol)return 3000;
                    if(has_length && length!=value)return 3000;
                    length=value;has_length=true;
                }
                line=eol+2;
            }
            if(has_length && length!=expected)return 7000;
            body=(size_t)(end-wire)+4;headers=true;
        }
    }
}
void gateway_internet_run(const char *upstream,uint16_t port,const char *dns,unsigned target,
                         gateway_internet_result_t *r,void (*idle)(void))
{
    *r=(gateway_internet_result_t){.network.code={5000,5000,5000},.http_code=5000};
    if(target>=GATEWAY_INTERNET_TARGETS){r->network.code[0]=-EINVAL;return;}
    probe_dns(upstream,port,dns,targets[target].host,&r->ipv4,&r->network,idle);
    for(unsigned i=0;i<3;i++)if(r->network.code[i])return;
    int fd=-1,err=0;int64_t start=milliseconds(),deadline=start+4000;
    struct sockaddr_in server={.sin_family=AF_INET,.sin_port=htons(port)};inet_pton(AF_INET,upstream,&server.sin_addr);
    fd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(fd<0){err=-errno;goto done;}
    if((err=nonblock(fd)))goto done;
    if(connect(fd,(struct sockaddr *)&server,sizeof(server))){if(errno!=EINPROGRESS){err=-errno;goto done;}
        if((err=wait_fd(fd,true,deadline,idle)))goto done;
        int e=0;socklen_t size=sizeof(e);
        if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&e,&size)){err=-errno;goto done;}if(e){err=-e;goto done;}}
    uint8_t hello[]={5,1,0},reply[10],request[]={5,1,0,1,0,0,0,0,0,80};memcpy(request+4,&r->ipv4,4);
    if((err=exact(fd,hello,3,true,deadline,idle)) || (err=exact(fd,reply,2,false,deadline,idle)))goto done;
    if(reply[0]!=5){err=3000;goto done;}if(reply[1]){err=2000+reply[1];goto done;}
    if((err=exact(fd,request,10,true,deadline,idle)) || (err=exact(fd,reply,4,false,deadline,idle)))goto done;
    if(reply[0]!=5 || reply[2]){err=3000;goto done;}if(reply[1]){err=1000+reply[1];goto done;}
    if(reply[3]!=1){err=3000;goto done;}if((err=exact(fd,reply+4,6,false,deadline,idle)))goto done;
    char http[256];int bytes=snprintf(http,sizeof(http),"GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\nCache-Control: no-cache\r\n\r\n",targets[target].path,targets[target].host);
    if(bytes<0 || (size_t)bytes>=sizeof(http)){err=3000;goto done;}
    if((err=exact(fd,(uint8_t *)http,(size_t)bytes,true,deadline,idle)))goto done;
    err=http_response(fd,target,deadline,&r->http_status,idle);
done:
    r->http_code=err;r->http_ms=(uint32_t)(milliseconds()-start);if(fd>=0)close(fd);
}
