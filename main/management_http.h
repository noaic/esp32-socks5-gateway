#pragma once
#include "esp_http_server.h"

/* HTTP owner task only: bound each request without another task/allocation. */
esp_err_t management_http_open(httpd_handle_t server, int fd);
void management_http_close(httpd_handle_t server, int fd);
void management_http_begin(httpd_req_t *req);
int management_http_recv(httpd_req_t *req, char *buffer, size_t size);
esp_err_t management_http_finish(httpd_req_t *req, esp_err_t result);
