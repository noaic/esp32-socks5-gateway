/* Model lwIP's already-closed write-half response after performing host FIN.
 * Used only by the host regression executable via GNU ld --wrap. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
int __real_shutdown(int fd, int how);
int __wrap_shutdown(int fd, int how)
{
    int rc=__real_shutdown(fd,how);
    if (rc==0 && how==SHUT_WR && getenv("RELAY_TEST_SHUTDOWN_CLOSED")) {
        errno=ENOTCONN;
        return -1;
    }
    return rc;
}

/* Deterministic downstream saturation with data still in the relay queue. */
ssize_t __real_send(int fd, const void *data, size_t size, int flags);
ssize_t __wrap_send(int fd, const void *data, size_t size, int flags)
{
    if (getenv("RELAY_TEST_STALL_PAYLOAD") && size>=13 &&
        memcmp(data,"idle-blocked!",13)==0) {
        errno=EAGAIN; return -1;
    }
    return __real_send(fd,data,size,flags);
}
