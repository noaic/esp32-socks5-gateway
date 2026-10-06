#include "gateway_settings.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
static bool text(const char *p, size_t capacity, size_t minimum)
{
    const char *end=memchr(p,0,capacity);
    if (!end || (size_t)(end-p)<minimum) return false;
    for (;p<end;p++) if ((unsigned char)*p<32 || (unsigned char)*p==127) return false;
    return true;
}
static bool ip(const char *s,uint8_t ap_subnet)
{
    uint32_t n=0;
    for (unsigned part=0;part<4;part++) {
        if (*s<'0' || *s>'9') return false;
        if (*s=='0' && s[1]>='0' && s[1]<='9') return false; /* Avoid lwIP octal interpretation. */
        unsigned value=0,digits=0;
        do { value=value*10+(unsigned)(*s++-'0'); if (++digits>3 || value>255) return false; }
        while (*s>='0' && *s<='9');
        n=(n<<8)|value;
        if (part<3) { if (*s++!='.') return false; } else if (*s) return false;
    }
    unsigned first=n>>24;
    return first>0 && first<224 && first!=127 && (n>>8)!=(0xc0a800u|ap_subnet);
}
bool gateway_settings_valid(const gateway_settings_t *s)
{
    if (!s || !text(s->sta_ssid,sizeof(s->sta_ssid),0) ||
        !text(s->sta_password,sizeof(s->sta_password),0) ||
        !text(s->ap_ssid,sizeof(s->ap_ssid),1) ||
        !text(s->ap_password,sizeof(s->ap_password),8) ||
        !text(s->admin_password,sizeof(s->admin_password),12) ||
        !text(s->socks_ipv4,sizeof(s->socks_ipv4),1) ||
        !text(s->dns_ipv4,sizeof(s->dns_ipv4),1) ||
        !ip(s->socks_ipv4,s->ap_subnet) || !ip(s->dns_ipv4,s->ap_subnet) || !s->socks_port ||
        s->max_clients<1 || s->max_clients>2) return false;
    size_t len=strlen(s->sta_password);
    if (len && len<8) return false;
    if (len==64) for (unsigned i=0;i<64;i++)
        if (!isxdigit((unsigned char)s->sta_password[i])) return false;
    return true;
}

void gateway_settings_ap_ip(const gateway_settings_t *s,char out[16])
{
    snprintf(out,16,"192.168.%u.1",(unsigned)s->ap_subnet);
}
bool gateway_settings_ap_overlap(const gateway_settings_t *s,uint32_t sta_ip,uint32_t sta_mask)
{
    if (!sta_ip) return false;
    uint32_t shared=sta_mask&0xffffff00u;
    return (sta_ip&shared)==((0xc0a80001u|((uint32_t)s->ap_subnet<<8))&shared);
}
