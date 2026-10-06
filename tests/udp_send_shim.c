#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>

/* Linked only into the host harness: one transient UDP send failure exercises
 * the production pending queue without altering firmware or control TCP. */
ssize_t __real_send(int fd,const void *data,size_t length,int flags);
ssize_t __wrap_send(int fd,const void *data,size_t length,int flags)
{
    static bool injected;
    if (!injected && getenv("UDP_TEST_FIRST_SEND_EAGAIN")) {
        int type=0; socklen_t size=sizeof(type);
        if (!getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size) && type==SOCK_DGRAM) {
            injected=true; errno=EAGAIN; return -1;
        }
    }
    return __real_send(fd,data,length,flags);
}
