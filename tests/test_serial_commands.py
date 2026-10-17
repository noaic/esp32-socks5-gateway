"""Exercise the actual bounded serial parser, including hostile and split input."""
import pathlib,subprocess,tempfile,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
HARNESS=r'''
#include "serial_commands.h"
#include <assert.h>
#include <string.h>
static serial_command_t got[32];static char arguments[32][96];static unsigned count;
static void capture(serial_command_t cmd,const char *argument,void *ctx){assert(ctx==(void *)1);strcpy(arguments[count],argument);got[count++]=cmd;}
static serial_parser_t parser;
static void feed(const char *s,uint64_t now){serial_commands_feed(&parser,(const uint8_t *)s,strlen(s),now,capture,(void *)1);}
int main(void){
 feed("help\r",1000);feed("\n",1000);assert(count==1 && got[0]==SERIAL_HELP);
 feed("sta",2000);feed("tus\n",2000);assert(got[1]==SERIAL_STATUS);
 feed(" events \n",3000);assert(got[2]==SERIAL_EVENTS);
 feed("log\n",4000);assert(got[3]==SERIAL_LOG_GET);
 feed("log\t info \r\n",5000);assert(got[4]==SERIAL_LOG_INFO);
 feed("log warn\n",6000);assert(got[5]==SERIAL_LOG_WARN);
 feed("log off\n",7000);assert(got[6]==SERIAL_LOG_OFF);
 feed("status\nhelp\nhelp\n",7001);assert(count==10 && got[7]==SERIAL_STATUS && got[8]==SERIAL_HELP && got[9]==SERIAL_HELP);
 feed("statusx\b\n",8000);assert(got[10]==SERIAL_STATUS);
 feed("status\x1b[0m\n",9000);assert(got[11]==SERIAL_INVALID);
 char huge[200];memset(huge,'x',sizeof(huge));serial_commands_feed(&parser,(uint8_t *)huge,sizeof(huge),10000,capture,(void *)1);
 feed("\bhelp\n",10000);assert(got[12]==SERIAL_TOO_LONG);
 feed("help\n",11000);assert(got[13]==SERIAL_HELP);
 feed("log info extra\n",12000);assert(got[14]==SERIAL_UNKNOWN);
 feed(" \t\r\n\n",13000);assert(count==15);
 feed("reset-admin\n",14000);assert(got[15]==SERIAL_RESET_ADMIN);
 uint8_t binary[]={'h','e','l','p',0,'\n'};serial_commands_feed(&parser,binary,sizeof(binary),15000,capture,(void *)1);assert(got[16]==SERIAL_INVALID);
 feed("reset-network\n",16000);assert(got[17]==SERIAL_RESET_NETWORK);
 feed("confirm ABCD1234\n",17000);assert(got[18]==SERIAL_CONFIRM && !strcmp(arguments[18],"ABCD1234"));
 feed("cancel\n",18000);assert(got[19]==SERIAL_CANCEL);
 feed("confirm\n",19000);assert(got[20]==SERIAL_CONFIRM && !arguments[20][0]);
 feed("confirm\t abcd1234 extra\n",20000);assert(got[21]==SERIAL_CONFIRM && !strcmp(arguments[21],"abcd1234 extra"));
 feed("reset-network extra\n",21000);assert(got[22]==SERIAL_UNKNOWN);
 feed("reset-admin\nconfirm ABCD1234\nstatus\nhelp\nlog\nevents\ncancel\n",22000);
 assert(count==30 && got[23]==SERIAL_RESET_ADMIN && got[24]==SERIAL_BUSY && got[25]==SERIAL_STATUS &&
        got[26]==SERIAL_HELP && got[27]==SERIAL_LOG_GET && got[28]==SERIAL_EVENTS && got[29]==SERIAL_CANCEL);
 feed("confirm ABCD1234\n",22001);assert(count==30); /* Reads/cancel do not reset the change throttle. */
 feed("confirm ABCD1234\n",23000);assert(count==31 && got[30]==SERIAL_CONFIRM);
 return 0;
}
'''
class SerialTests(unittest.TestCase):
 def test_terminal_echo_prompt_backspace_empty_crlf_and_rejected_input(self):
  terminal=r'''
#include "serial_commands.h"
#include <assert.h>
#include <string.h>
static char output[2048];static size_t used;static serial_command_t got;static unsigned count;
static void write_text(const char *s,size_t n,void *ctx){(void)ctx;assert(used+n<sizeof(output));memcpy(output+used,s,n);used+=n;output[used]=0;}
static void prompt(void *ctx){write_text("gateway> ",9,ctx);}
static void dispatch(serial_command_t c,const char *arg,void *ctx){(void)arg;(void)ctx;got=c;count++;write_text("OK\n",3,ctx);}
int main(void){
 serial_parser_t p={0};serial_terminal_t t={.write=write_text,.prompt=prompt};
 const char *typed="statusx\b\r\n";
 serial_commands_feed_terminal(&p,(const uint8_t *)typed,strlen(typed),1000,dispatch,NULL,&t);
 assert(got==SERIAL_STATUS&&count==1&&!strcmp(output,"statusx\b \b\nOK\ngateway> "));
 used=0;output[0]=0;serial_commands_feed_terminal(&p,(uint8_t *)"\n",1,2000,dispatch,NULL,&t);
 assert(count==1&&!strcmp(output,"\ngateway> "));
 used=0;output[0]=0;const uint8_t invalid[]={'h','e',27,'[','0','m','\n'};
 serial_commands_feed_terminal(&p,invalid,sizeof(invalid),3000,dispatch,NULL,&t);
 assert(got==SERIAL_INVALID&&!strcmp(output,"he\nOK\ngateway> ")&&!strchr(output,27));
 used=0;output[0]=0;char longline[150];memset(longline,'x',sizeof(longline));longline[149]='\n';
 serial_commands_feed_terminal(&p,(uint8_t *)longline,sizeof(longline),4000,dispatch,NULL,&t);
 assert(got==SERIAL_TOO_LONG&&used==95+1+3+9);
 serial_commands_feed_terminal(&p,(uint8_t *)"help\n",5,4001,dispatch,NULL,&t);assert(got==SERIAL_HELP&&count==4);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'check.c').write_text(terminal)
   subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/serial_commands.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
 def test_split_lines_crlf_backspace_invalid_overflow_rate_and_commands(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'check.c').write_text(HARNESS)
   subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/serial_commands.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
