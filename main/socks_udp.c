#include "socks_udp.h"
#include <string.h>
size_t socks_udp_encode(uint8_t *out, size_t capacity, uint32_t ipv4, uint16_t port,
                        const uint8_t *payload, size_t size)
{
    if (!port || size > 65507 - 10 || capacity < size + 10) return 0;
    memset(out,0,3); out[3]=1; memcpy(out+4,&ipv4,4);
    out[8]=port>>8; out[9]=port;
    if (size) memcpy(out+10,payload,size);
    return size+10;
}
bool socks_udp_decode(const uint8_t *data, size_t size, uint32_t *ipv4,
                      uint16_t *port, const uint8_t **payload, size_t *length)
{
    if (size < 10 || size > 65507 || data[0] || data[1] || data[2] || data[3]!=1) return false;
    *port=(uint16_t)data[8]<<8 | data[9]; if (!*port) return false;
    memcpy(ipv4,data+4,4); *payload=data+10; *length=size-10;
    return true;
}
