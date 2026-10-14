#include "serial_console.h"
#include "serial_commands.h"
#include "serial_recovery.h"
#include "gateway_settings.h"
#include "esp_random.h"
#include "gateway_state.h"
#include "gateway_control.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "driver/uart.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdatomic.h>
static serial_parser_t parser;
static serial_recovery_t recovery;
static bool ready;
static bool prompt_shown;
static atomic_bool startup_complete;
static bool log_selected,quiet_applied;
static uint64_t quiet_after;
static esp_log_level_t level=ESP_LOG_INFO;
static const char *const names[]={"boot","ap_up","ap_down","sta_link","sta_ip","sta_down",
    "sta_lost_ip","subnet_conflict","client_join","client_leave","config_stage","config_confirm",
    "config_rollback","password_set","maint_start","maint_done","diag_done","tcp_error","udp_error",
    "led_setting","led_error","internet_result","internet_setting","recovery_commit"};
static const char *level_name(void){return level==ESP_LOG_INFO?"info":level==ESP_LOG_WARN?"warn":"off";}
static void terminal_write(const char *text,size_t length,void *context)
{(void)context;fwrite(text,1,length,stdout);fflush(stdout);}
static void terminal_prompt(void *context)
{(void)context;fputs("gateway> ",stdout);fflush(stdout);prompt_shown=true;}
static void dispatch(serial_command_t command,const char *argument,void *context)
{
    (void)context;
    switch(command) {
    case SERIAL_HELP:
        puts("help        Show commands\nstatus      Show status and memory\nevents      Show last 8 events\nlog         Show log level\nlog info    Enable live info logs\nlog warn    Show warnings and errors\nlog off     Disable app logs\nreset-admin Clear admin; keep network\nreset-network Restore network; keep admin\nconfirm CODE Confirm reset within 30 seconds\ncancel      Cancel pending reset");break;
    case SERIAL_STATUS: {
        gateway_state_t s;gateway_state_snapshot(&s);
        gateway_control_snapshot_t c;gateway_control_snapshot(&c);
        static const char *const internet[]={"unknown","checking","pass","fail","expired"};
        uint64_t now=gateway_state_clock_ms();
        printf("uptime_s=%llu reset=%d\nap=%s sta_ip=%s conflict=%d\n",
            (unsigned long long)(now/1000),esp_reset_reason(),s.ap_up?"up":"down",s.sta_ip?"ready":"not_ready",s.subnet_conflict);
        printf("internet=%s failures=%lu auto=%d\ninternal_free=%u internal_min=%u internal_largest=%u\npsram_free=%u\n",
            internet[gateway_internet_status(&s,now)],(unsigned long)s.internet_failures,c.internet_auto,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        printf("worker_busy=%d stack_free_min=%u stack_total=6144 log=%s\n",c.running,
            (unsigned)c.stack_min,level_name());break;
    }
    case SERIAL_EVENTS: {
        gateway_event_t events[8];size_t count=gateway_events_snapshot(events,8);
        puts("Recent events (oldest first):");
        for(size_t i=0;i<count;i++) {
            unsigned kind=(unsigned)events[i].kind;
            printf("%lu +%llus %s code=%ld count=%lu\n",(unsigned long)events[i].sequence,
                (unsigned long long)(events[i].last_ms/1000),kind<sizeof(names)/sizeof(*names)?names[kind]:"unknown",
                (long)events[i].detail,(unsigned long)events[i].count);
        }
        if(!count)puts("No events.");
        break;
    }
    case SERIAL_RESET_ADMIN:case SERIAL_RESET_NETWORK: {
        gateway_control_snapshot_t current;gateway_control_snapshot(&current);
        serial_recovery_cancel(&recovery);
        if(!current.available || current.running){puts("Busy. Try again.");break;}
        bool network=command==SERIAL_RESET_NETWORK;
        serial_recovery_begin(&recovery,network,esp_random(),gateway_state_clock_ms());
        puts(network?"Restore compiled network defaults. Keep admin. Cancel trial. Reboot.":
            "Clear admin password. Keep current network. Cancel trial. Reboot.");
        puts(network?"AP subnet becomes 192.168.4.0/24. Compiled Wi-Fi credentials apply.":
            "Set a new password on the AP Web page after reboot.");
        printf("Type confirm %08lX within 30 seconds, or cancel.\n",(unsigned long)recovery.token);break;
    }
    case SERIAL_CANCEL:serial_recovery_cancel(&recovery);puts("OK cancelled.");break;
    case SERIAL_CONFIRM: {
        bool network=false;
        serial_confirm_result_t result=serial_recovery_confirm(&recovery,argument,gateway_state_clock_ms(),&network);
        if(result!=SERIAL_CONFIRM_OK) {
            puts(result==SERIAL_CONFIRM_NONE?"No pending reset.":result==SERIAL_CONFIRM_EXPIRED?"Confirmation expired. Start again.":"Invalid confirmation. Reset cancelled.");
            break;
        }
        esp_err_t err=gateway_control_submit(network?GW_ACTION_RESET_NETWORK:GW_ACTION_RESET_ADMIN);
        puts(err==ESP_OK?"Reset queued. Connections will close.":"Busy. Reset cancelled. Start again.");break;
    }
    case SERIAL_LOG_INFO:case SERIAL_LOG_WARN:case SERIAL_LOG_OFF:
        level=command==SERIAL_LOG_INFO?ESP_LOG_INFO:command==SERIAL_LOG_WARN?ESP_LOG_WARN:ESP_LOG_NONE;
        esp_log_level_set("*",level);log_selected=true;
        printf("OK log=%s\n",level_name());break;
    case SERIAL_LOG_GET:printf("log=%s\n",level_name());break;
    case SERIAL_TOO_LONG:puts("Line too long.");break;
    case SERIAL_INVALID:puts("Invalid input.");break;
    case SERIAL_BUSY:puts("Busy. Wait 1 second.");break;
    default:puts("Unknown command. Type help.");break;
    }
    fflush(stdout);
}
esp_err_t serial_console_start(void)
{
    /* Preserve UART0 console baud/pins. Install RX buffering only; stdout stays on
     * its existing VFS path. No second task, RX consumer, or TX ring. */
    int port=CONFIG_ESP_CONSOLE_UART_NUM;
    if(!uart_is_driver_installed(port)) {
        esp_err_t err=uart_driver_install(port,256,0,0,NULL,0);
        if(err!=ESP_OK)return err;
    }
    ready=true;return ESP_OK;
}
void serial_console_startup_complete(void){atomic_store(&startup_complete,true);}
void serial_console_poll(void)
{
    if(!ready)return;
    uint64_t now=gateway_state_clock_ms();
    if(!quiet_after && atomic_load(&startup_complete))quiet_after=now+10000;
    if(!quiet_applied && quiet_after && now>=quiet_after) {
        quiet_applied=true;
        if(!log_selected){level=ESP_LOG_WARN;esp_log_level_set("*",level);}
        if(prompt_shown)fputc('\n',stdout);
        printf("Ready. Type help. Log: %s.\n",level_name());fflush(stdout);
        terminal_prompt(NULL);
        if(parser.used && !parser.discard)terminal_write(parser.line,parser.used,NULL);
    }
    uint8_t bytes[64];int n=uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM,bytes,sizeof(bytes),0);
    if(n>0) {
        if(!prompt_shown)terminal_prompt(NULL);
        const serial_terminal_t terminal={.write=terminal_write,.prompt=terminal_prompt};
        serial_commands_feed_terminal(&parser,bytes,(size_t)n,gateway_state_clock_ms(),dispatch,NULL,&terminal);
    }
}
