#pragma once
#include <stddef.h>
#include "cJSON.h"
#include "gateway_settings.h"
/* data[used] must be a terminator in a buffer of at least used+1 bytes. */
cJSON *management_json_parse(const char *data,size_t used);
bool management_number(const cJSON *value,unsigned low,unsigned high);
/* Password blanks retain previous values; sta_open explicitly clears STA password. */
bool management_settings_patch(const cJSON *root,gateway_settings_t *next);
