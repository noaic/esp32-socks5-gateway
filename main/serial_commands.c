#include "serial_commands.h"
#include <string.h>
static serial_command_t parse(char *line,const char **argument)
{
    char *begin=line;while(*begin==' '||*begin=='\t')begin++;
    char *end=begin+strlen(begin);while(end>begin && (end[-1]==' '||end[-1]=='\t'))*--end=0;
    if(!strcmp(begin,"reset-admin"))return SERIAL_RESET_ADMIN;
    if(!strcmp(begin,"reset-network"))return SERIAL_RESET_NETWORK;
    if(!strcmp(begin,"cancel"))return SERIAL_CANCEL;
    if(!strcmp(begin,"confirm")){*argument="";return SERIAL_CONFIRM;}
    if(!strncmp(begin,"confirm",7) && (begin[7]==' '||begin[7]=='\t')) {
        begin+=7;while(*begin==' '||*begin=='\t')begin++;
        *argument=begin;return SERIAL_CONFIRM;
    }
    if(!strcmp(begin,"help"))return SERIAL_HELP;
    if(!strcmp(begin,"status"))return SERIAL_STATUS;
    if(!strcmp(begin,"events"))return SERIAL_EVENTS;
    if(!strcmp(begin,"log"))return SERIAL_LOG_GET;
    if(!strncmp(begin,"log",3) && (begin[3]==' '||begin[3]=='\t')) {
        begin+=3;while(*begin==' '||*begin=='\t')begin++;
        if(!strcmp(begin,"info"))return SERIAL_LOG_INFO;
        if(!strcmp(begin,"warn"))return SERIAL_LOG_WARN;
        if(!strcmp(begin,"off"))return SERIAL_LOG_OFF;
    }
    return SERIAL_UNKNOWN;
}
void serial_commands_feed_terminal(serial_parser_t *p,const uint8_t *bytes,size_t length,
                          uint64_t now,serial_dispatch_t dispatch,void *context,
                          const serial_terminal_t *terminal)
{
    for(size_t i=0;i<length;i++) {
        unsigned c=bytes[i];
        if(c=='\n' && p->previous_cr){p->previous_cr=false;continue;}
        p->previous_cr=c=='\r';
        if(c=='\r'||c=='\n') {
            if(terminal && terminal->write)terminal->write("\n",1,context);
            p->line[p->used]=0;
            bool empty=!p->discard;for(size_t n=0;n<p->used;n++)if(p->line[n]!=' ' && p->line[n]!='\t')empty=false;
            if(!empty) {
                if(now>=p->next_ms) {
                    p->busy_reported=false;p->next_ms=now+1000;
                    const char *argument="";
                    serial_command_t command=p->discard?(p->overflow?SERIAL_TOO_LONG:SERIAL_INVALID):parse(p->line,&argument);
                    dispatch(command,argument,context);
                } else if(!p->busy_reported){p->busy_reported=true;dispatch(SERIAL_BUSY,"",context);}
            }
            p->used=0;p->discard=p->overflow=false;
            if(terminal && terminal->prompt)terminal->prompt(context);
            continue;
        }
        if(p->discard)continue;
        if(c==8||c==127){if(p->used){p->used--;if(terminal && terminal->write)terminal->write("\b \b",3,context);}continue;}
        if((c<32 && c!='\t')||c>126){p->discard=true;continue;}
        if(p->used>=sizeof(p->line)-1){p->discard=p->overflow=true;continue;}
        p->line[p->used++]=(char)c;
        if(terminal && terminal->write){char visible=c=='\t'?' ':(char)c;terminal->write(&visible,1,context);}
    }
}
void serial_commands_feed(serial_parser_t *p,const uint8_t *bytes,size_t length,
                          uint64_t now,serial_dispatch_t dispatch,void *context)
{
    serial_commands_feed_terminal(p,bytes,length,now,dispatch,context,NULL);
}
