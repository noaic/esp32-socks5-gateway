#include "serial_recovery.h"
#include <string.h>
void serial_recovery_begin(serial_recovery_t *s,bool network,uint32_t token,uint64_t now)
{*s=(serial_recovery_t){.token=token,.expires_ms=now+SERIAL_CONFIRM_TTL_MS,.network=network,.active=true};}
void serial_recovery_cancel(serial_recovery_t *s){*s=(serial_recovery_t){0};}
serial_confirm_result_t serial_recovery_confirm(serial_recovery_t *s,const char *text,uint64_t now,bool *network)
{
    serial_recovery_t previous=*s;serial_recovery_cancel(s);
    if(!previous.active)return SERIAL_CONFIRM_NONE;
    if(now>=previous.expires_ms)return SERIAL_CONFIRM_EXPIRED;
    if(!text || strlen(text)!=8)return SERIAL_CONFIRM_WRONG;
    uint32_t token=0;
    for(unsigned i=0;i<8;i++) {
        unsigned c=(unsigned char)text[i],digit;
        if(c>='0' && c<='9')digit=c-'0';
        else if(c>='a' && c<='f')digit=c-'a'+10;
        else if(c>='A' && c<='F')digit=c-'A'+10;
        else return SERIAL_CONFIRM_WRONG;
        token=(token<<4)|digit;
    }
    if(token!=previous.token)return SERIAL_CONFIRM_WRONG;
    *network=previous.network;return SERIAL_CONFIRM_OK;
}
