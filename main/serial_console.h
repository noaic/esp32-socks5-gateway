#pragma once
#include "esp_err.h"
esp_err_t serial_console_start(void);
/* Only called from the maintenance worker, including its network wait callback. */
void serial_console_poll(void);
/* Called after normal initialization; log commands issued earlier take precedence. */
void serial_console_startup_complete(void);
