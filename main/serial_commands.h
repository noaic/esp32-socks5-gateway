#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { SERIAL_LINE_CAPACITY=96 };
typedef enum { SERIAL_HELP,SERIAL_STATUS,SERIAL_EVENTS,SERIAL_LOG_GET,SERIAL_LOG_INFO,
    SERIAL_LOG_WARN,SERIAL_LOG_OFF,SERIAL_UNKNOWN,SERIAL_TOO_LONG,SERIAL_INVALID,SERIAL_BUSY,SERIAL_RESET_ADMIN,SERIAL_RESET_NETWORK,SERIAL_CONFIRM,SERIAL_CANCEL } serial_command_t;
typedef struct {
    char line[SERIAL_LINE_CAPACITY];
    size_t used;
    uint64_t next_ms;
    bool discard,overflow,previous_cr,busy_reported;
} serial_parser_t;
typedef void (*serial_dispatch_t)(serial_command_t command,const char *argument,void *context);
typedef struct {
    void (*write)(const char *text,size_t length,void *context);
    void (*prompt)(void *context);
} serial_terminal_t;
/* Bounded ASCII lines; changes are limited to one per second. Reads/cancel are immediate.
 * No heap or terminal escape handling. */
void serial_commands_feed(serial_parser_t *parser,const uint8_t *bytes,size_t length,
                          uint64_t now,serial_dispatch_t dispatch,void *context);
void serial_commands_feed_terminal(serial_parser_t *parser,const uint8_t *bytes,size_t length,
                          uint64_t now,serial_dispatch_t dispatch,void *context,
                          const serial_terminal_t *terminal);
