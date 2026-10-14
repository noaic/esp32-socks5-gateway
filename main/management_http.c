#include "management_http.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include <errno.h>
#include <unistd.h>

#ifndef MANAGEMENT_HTTP_REQUEST_US
#define MANAGEMENT_HTTP_REQUEST_US 5000000LL
#endif
/* Matches management's max_open_sockets. Only its HTTP task touches these. */
static struct { int fd; int64_t deadline; size_t unread; } sessions[2] = {{.fd=-1},{.fd=-1}};
static int slot(int fd)
{
    for (unsigned i=0;i<2;i++) if (sessions[i].fd==fd) return (int)i;
    return -1;
}
static bool remaining(int index, int option)
{
    int64_t now=esp_timer_get_time();
    if (!sessions[index].deadline) sessions[index].deadline=now+MANAGEMENT_HTTP_REQUEST_US;
    int64_t left=sessions[index].deadline-now;
    /* lwIP truncates timeval to milliseconds; zero disables the timeout.
     * Treat the final sub-millisecond budget as expired rather than allowing
     * rounding to turn a bounded request into an unbounded socket wait. */
    if (left<1000) return false;
    if (left>3000000) left=3000000;
    struct timeval timeout={.tv_sec=left/1000000,.tv_usec=left%1000000};
    return setsockopt(sessions[index].fd,SOL_SOCKET,option,&timeout,sizeof(timeout))==0;
}
static int receive(httpd_handle_t server,int fd,char *buffer,size_t size,int flags)
{
    (void)server;
    int index=slot(fd);
    if (index<0) return HTTPD_SOCK_ERR_FAIL;
    if (!remaining(index,SO_RCVTIMEO)) return HTTPD_SOCK_ERR_TIMEOUT;
    /* httpd_socket_recv calls the override recursively; use the socket here. */
    int n=recv(fd,buffer,size,flags);
    return n>=0?n:(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)?
        HTTPD_SOCK_ERR_TIMEOUT:HTTPD_SOCK_ERR_FAIL;
}
static int transmit(httpd_handle_t server,int fd,const char *buffer,size_t size,int flags)
{
    (void)server;
    int index=slot(fd);
    if (index<0) return HTTPD_SOCK_ERR_FAIL;
    if (!remaining(index,SO_SNDTIMEO)) return HTTPD_SOCK_ERR_TIMEOUT;
    int n=send(fd,buffer,size,flags);
    if(n==0 && size)return HTTPD_SOCK_ERR_FAIL;
    return n>=0?n:(errno==EAGAIN || errno==EWOULDBLOCK || errno==EINTR)?
        HTTPD_SOCK_ERR_TIMEOUT:HTTPD_SOCK_ERR_FAIL;
}
esp_err_t management_http_open(httpd_handle_t server,int fd)
{
    for (unsigned i=0;i<2;i++) if (sessions[i].fd<0) {
        sessions[i].fd=fd;sessions[i].deadline=0;sessions[i].unread=0;
        esp_err_t result=httpd_sess_set_recv_override(server,fd,receive);
        if (result==ESP_OK) result=httpd_sess_set_send_override(server,fd,transmit);
        if (result!=ESP_OK) sessions[i].fd=-1;
        return result;
    }
    return ESP_FAIL;
}
void management_http_close(httpd_handle_t server,int fd)
{
    (void)server;
    int index=slot(fd);if(index>=0)sessions[index].fd=-1;
    close(fd);
}
void management_http_begin(httpd_req_t *req)
{
    int index=slot(httpd_req_to_sockfd(req));
    if (index>=0) sessions[index].unread=req->content_len;
}
int management_http_recv(httpd_req_t *req,char *buffer,size_t size)
{
    int index=slot(httpd_req_to_sockfd(req));
    if (index<0) return HTTPD_SOCK_ERR_FAIL;
    int n=httpd_req_recv(req,buffer,size);
    if (n>0) {
        if ((size_t)n>sessions[index].unread) return HTTPD_SOCK_ERR_FAIL;
        sessions[index].unread-=n;
    }
    return n;
}
esp_err_t management_http_finish(httpd_req_t *req,esp_err_t result)
{
    int index=slot(httpd_req_to_sockfd(req));
    /* Failure closes directly, skipping SDK leftover-body purging. A close
     * queued on the same blocked HTTP task would not provide this protection. */
    if (result!=ESP_OK || index<0 || sessions[index].unread ||
        (sessions[index].deadline && esp_timer_get_time()>=sessions[index].deadline)) return ESP_FAIL;
    sessions[index].deadline=0;
    return ESP_OK;
}
