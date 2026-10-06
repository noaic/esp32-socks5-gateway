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
 feed("status\nhelp\nhelp\n",7001);assert(count==8 && got[7]==SERIAL_BUSY);
 feed("statusx\b\n",8000);assert(got[8]==SERIAL_STATUS);
 feed("status\x1b[0m\n",9000);assert(got[9]==SERIAL_INVALID);
 char huge[200];memset(huge,'x',sizeof(huge));serial_commands_feed(&parser,(uint8_t *)huge,sizeof(huge),10000,capture,(void *)1);
 feed("\bhelp\n",10000);assert(got[10]==SERIAL_TOO_LONG);
 feed("help\n",11000);assert(got[11]==SERIAL_HELP);
 feed("log info extra\n",12000);assert(got[12]==SERIAL_UNKNOWN);
 feed(" \t\r\n\n",13000);assert(count==13);
 feed("reset-admin\n",14000);assert(got[13]==SERIAL_RESET_ADMIN);
 uint8_t binary[]={'h','e','l','p',0,'\n'};serial_commands_feed(&parser,binary,sizeof(binary),15000,capture,(void *)1);assert(got[14]==SERIAL_INVALID);
 feed("reset-network\n",16000);assert(got[15]==SERIAL_RESET_NETWORK);
 feed("confirm ABCD1234\n",17000);assert(got[16]==SERIAL_CONFIRM && !strcmp(arguments[16],"ABCD1234"));
 feed("cancel\n",18000);assert(got[17]==SERIAL_CANCEL);
 feed("confirm\n",19000);assert(got[18]==SERIAL_CONFIRM && !arguments[18][0]);
 feed("confirm\t abcd1234 extra\n",20000);assert(got[19]==SERIAL_CONFIRM && !strcmp(arguments[19],"abcd1234 extra"));
 feed("reset-network extra\n",21000);assert(got[20]==SERIAL_UNKNOWN);
 return 0;
}
'''
class SerialTests(unittest.TestCase):
 def test_split_lines_crlf_backspace_invalid_overflow_rate_and_commands(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'check.c').write_text(HARNESS)
   subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/serial_commands.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
