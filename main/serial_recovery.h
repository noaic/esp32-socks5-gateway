#pragma once
#include <stdbool.h>
#include <stdint.h>
enum { SERIAL_CONFIRM_TTL_MS=30000 };
typedef struct {uint32_t token;uint64_t expires_ms;bool network,active;} serial_recovery_t;
typedef enum { SERIAL_CONFIRM_OK,SERIAL_CONFIRM_NONE,SERIAL_CONFIRM_EXPIRED,SERIAL_CONFIRM_WRONG } serial_confirm_result_t;
void serial_recovery_begin(serial_recovery_t *state,bool network,uint32_t token,uint64_t now);
void serial_recovery_cancel(serial_recovery_t *state);
/* Every attempt consumes the challenge, including wrong/malformed tokens. */
serial_confirm_result_t serial_recovery_confirm(serial_recovery_t *state,const char *text,uint64_t now,bool *network);
