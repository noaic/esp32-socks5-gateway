#pragma once
#include "esp_http_server.h"
esp_err_t diagnostic_sta_run(httpd_req_t *req, const unsigned char *pattern,
                             unsigned char *buffer, size_t block, size_t capacity);
