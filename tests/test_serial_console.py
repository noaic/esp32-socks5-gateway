"""Exercise the actual console poll and terminal callbacks through a raw PTY."""
import os,pathlib,pty,select,subprocess,tempfile,time,tty,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class ConsoleTests(unittest.TestCase):
 def test_firmware_echo_before_enter_backspace_and_prompt(self):
  source=(ROOT/'main/serial_console.c').read_text()
  callbacks='static void terminal_write'+source.split('static void terminal_write',1)[1].split('static void dispatch',1)[0]
  poll='void serial_console_poll'+source.split('void serial_console_poll',1)[1]
  harness=r'''
#include "serial_commands.h"
#include <stdio.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
static serial_parser_t parser;static bool ready=true,prompt_shown,log_selected,quiet_applied;
static atomic_bool startup_complete=true;static uint64_t quiet_after;static int level;
#define ESP_LOG_WARN 2
#define CONFIG_ESP_CONSOLE_UART_NUM 0
static uint64_t gateway_state_clock_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void esp_log_level_set(const char *tag,int value){(void)tag;(void)value;}
static const char *level_name(void){return "warn";}
static int uart_read_bytes(int port,uint8_t *bytes,size_t size,int wait){(void)port;(void)wait;int n=read(0,bytes,size);return n>0?n:0;}
__CALLBACKS__
static void dispatch(serial_command_t command,const char *argument,void *context){(void)argument;(void)context;printf("command=%d\n",command);fflush(stdout);}
__POLL__
int main(void){fcntl(0,F_SETFL,O_NONBLOCK);quiet_after=1;for(unsigned i=0;i<500;i++){serial_console_poll();struct timespec t={.tv_nsec=5000000};nanosleep(&t,NULL);}return 0;}
'''.replace('__CALLBACKS__',callbacks).replace('__POLL__',poll)
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'check.c').write_text(harness)
   subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/serial_commands.c'),'-o',str(p/'check')],check=True)
   master,slave=pty.openpty();tty.setraw(slave);process=subprocess.Popen([str(p/'check')],stdin=slave,stdout=slave,stderr=subprocess.PIPE)
   seen=b''
   def until(needle):
    nonlocal seen
    deadline=time.monotonic()+2
    while needle not in seen and time.monotonic()<deadline:
     if select.select([master],[],[],.05)[0]:seen+=os.read(master,2048)
    self.assertIn(needle,seen)
   try:
    until(b'gateway> ');os.write(master,b'he');until(b'gateway> he')
    os.write(master,b'x\x7flp\r\n');until(b'hex\b \blp\ncommand=0\ngateway> ')
    self.assertEqual(seen.count(b'command=0'),1)
   finally:
    process.terminate();process.communicate(timeout=3);os.close(master);os.close(slave)
