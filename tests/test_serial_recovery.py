"""Confirmation lifecycle and actual production NVS recovery across trials/races."""
import pathlib,subprocess,tempfile,unittest
from test_gateway_settings import SDK,LIFECYCLE,ROOT
CHALLENGE=r'''
#include "serial_recovery.h"
#include <assert.h>
int main(void){
 serial_recovery_t s={0};bool network=false;
 assert(serial_recovery_confirm(&s,"12345678",0,&network)==SERIAL_CONFIRM_NONE);
 serial_recovery_begin(&s,false,0x1234abcd,1000);
 assert(serial_recovery_confirm(&s,"1234ABCD",2000,&network)==SERIAL_CONFIRM_OK && !network);
 assert(serial_recovery_confirm(&s,"1234ABCD",2001,&network)==SERIAL_CONFIRM_NONE);
 serial_recovery_begin(&s,true,0,1000);
 assert(serial_recovery_confirm(&s,"00000000",30999,&network)==SERIAL_CONFIRM_OK && network);
 serial_recovery_begin(&s,true,0,1000);
 assert(serial_recovery_confirm(&s,"00000000",31000,&network)==SERIAL_CONFIRM_EXPIRED);
 serial_recovery_begin(&s,false,0xdeadbeef,0);
 serial_recovery_begin(&s,true,0x12345678,1000);
 assert(serial_recovery_confirm(&s,"deadbeef",1001,&network)==SERIAL_CONFIRM_WRONG);
 assert(serial_recovery_confirm(&s,"12345678",1002,&network)==SERIAL_CONFIRM_NONE);
 const char *invalid[]={"1234567","123456789","1234567g","0x123456","12345678 extra","",0};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(*invalid);i++){
  serial_recovery_begin(&s,true,0x12345678,0);
  assert(serial_recovery_confirm(&s,invalid[i],10,&network)==SERIAL_CONFIRM_WRONG);
  assert(!s.active);
 }
 serial_recovery_begin(&s,true,0xffffffff,1000);serial_recovery_cancel(&s);
 assert(serial_recovery_confirm(&s,"ffffffff",1001,&network)==SERIAL_CONFIRM_NONE);
 return 0;
}
'''
RECOVERY=r'''
static void check_record(bool bootstrap,unsigned subnet,const char *password){
 legacy_record_layout r;assert(persistent_size==sizeof(r));memcpy(&r,persisted,sizeof(r));
 assert(r.version==2 && !r.trial && r.bootstrap==bootstrap && r.previous_bootstrap==bootstrap);
 assert(!memcmp(&r.active,&r.previous,sizeof(r.active)) && r.active.ap_subnet==subnet);
 assert(!strcmp(r.active.admin_password,password));
 if(bootstrap){for(unsigned i=0;i<sizeof(r.active.admin_password);i++)assert(!r.active.admin_password[i]);}
}
static pthread_barrier_t barrier;
static int confirm_error,recover_error;
static void *confirmer(void *unused){(void)unused;pthread_barrier_wait(&barrier);confirm_error=gateway_settings_confirm();return 0;}
static void *recoverer(void *unused){(void)unused;pthread_barrier_wait(&barrier);recover_error=gateway_settings_recover(false,NULL);return 0;}
int main(void){
 char ip[16];unsigned char before[600];
 assert(gateway_settings_init()==ESP_OK);
 assert(gateway_settings_setup_password("original-password")==ESP_OK);
 gateway_settings_t next=*gateway_settings();next.ap_subnet=10;strcpy(next.admin_password,"trial-password");
 assert(gateway_settings_stage(&next)==ESP_OK);
 memcpy(before,persisted,600);
 assert(gateway_settings_recover(false,ip)==ESP_ERR_INVALID_STATE);assert(!memcmp(before,persisted,600)); /* Restart already staged. */
 assert(gateway_settings_init()==ESP_OK && gateway_settings_pending());
 memcpy(before,persisted,600);fail_commit=true;
 assert(gateway_settings_recover(false,ip)==ESP_FAIL);
 assert(gateway_settings_pending() && !gateway_settings_needs_setup() && !memcmp(before,persisted,600));fail_commit=false;
 assert(gateway_settings_recover(false,ip)==ESP_OK && !strcmp(ip,"192.168.10.1"));
 check_record(true,10,"");assert(!gateway_settings_pending());
 assert(!gateway_settings_needs_setup() && !strcmp(gateway_settings()->admin_password,"trial-password")); /* Live auth stays stable until reboot. */
 assert(gateway_settings_confirm()==ESP_ERR_INVALID_STATE && gateway_settings_rollback()==ESP_ERR_INVALID_STATE);
 assert(gateway_settings_stage(&next)==ESP_ERR_INVALID_STATE && gateway_settings_recover(true,ip)==ESP_ERR_INVALID_STATE);
 assert(gateway_settings_init()==ESP_OK && gateway_settings_needs_setup() && !gateway_settings()->admin_password[0]);
 assert(gateway_settings_init()==ESP_OK && gateway_settings_needs_setup());check_record(true,10,"");
 assert(gateway_settings_setup_password("new-private-password")==ESP_OK);
 next=*gateway_settings();next.ap_subnet=20;strcpy(next.admin_password,"another-trial-password");
 assert(gateway_settings_stage(&next)==ESP_OK && gateway_settings_init()==ESP_OK && gateway_settings_pending());
 memcpy(before,persisted,600);fail_commit=true;assert(gateway_settings_recover(true,ip)==ESP_FAIL);assert(!memcmp(before,persisted,600));fail_commit=false;
 assert(gateway_settings_recover(true,ip)==ESP_OK && !strcmp(ip,"192.168.4.1"));
 check_record(false,4,"another-trial-password");
 assert(gateway_settings_init()==ESP_OK && !gateway_settings_pending() && !gateway_settings_needs_setup());
 assert(!strcmp(gateway_settings()->sta_ssid,CONFIG_PROXY_STA_SSID) && gateway_settings()->socks_port==7897);
 assert(gateway_settings_init()==ESP_OK && !strcmp(gateway_settings()->admin_password,"another-trial-password"));
 assert(gateway_settings_recover(false,NULL)==ESP_OK && gateway_settings_init()==ESP_OK);
 assert(gateway_settings_recover(true,NULL)==ESP_OK && gateway_settings_init()==ESP_OK && gateway_settings_needs_setup());check_record(true,4,"");
 for(unsigned i=0;i<20;i++){
  assert(gateway_settings_setup_password("race-test-password")==ESP_OK);
  next=*gateway_settings();next.ap_subnet=10;
  assert(gateway_settings_stage(&next)==ESP_OK && gateway_settings_init()==ESP_OK && gateway_settings_pending());
  assert(!pthread_barrier_init(&barrier,NULL,2));pthread_t a,b;
  assert(!pthread_create(&a,NULL,confirmer,NULL));assert(!pthread_create(&b,NULL,recoverer,NULL));
  pthread_join(a,NULL);pthread_join(b,NULL);pthread_barrier_destroy(&barrier);
  assert(recover_error==ESP_OK && (confirm_error==ESP_OK || confirm_error==ESP_ERR_INVALID_STATE));
  check_record(true,10,"");assert(gateway_settings_init()==ESP_OK && gateway_settings_needs_setup());
 }
 return 0;
}
'''
class RecoveryTests(unittest.TestCase):
 def test_bound_single_use_expiring_confirmation(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'check.c').write_text(CHALLENGE)
   subprocess.run(['gcc','-std=c11','-Wall','-Wextra','-Werror','-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/serial_recovery.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
 def test_actual_nvs_recovery_commit_failure_trial_password_backup_and_concurrent_confirm(self):
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'mock_sdk.h').write_text(SDK);(p/'freertos').mkdir()
   for h in ['esp_err.h','sdkconfig.h','esp_log.h','nvs.h','freertos/FreeRTOS.h','freertos/semphr.h']:(p/h).write_text('#include "mock_sdk.h"\n')
   (p/'check.c').write_text(LIFECYCLE.split('int main(void)')[0]+RECOVERY)
   subprocess.run(['gcc','-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-Werror','-Wno-unused-function','-pthread','-DESP_PLATFORM','-I',str(p),'-I',str(ROOT/'main'),str(p/'check.c'),str(ROOT/'main/gateway_settings.c'),str(ROOT/'main/settings_validation.c'),'-o',str(p/'check')],check=True)
   subprocess.run([str(p/'check')],check=True)
