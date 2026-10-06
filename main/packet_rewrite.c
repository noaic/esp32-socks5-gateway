#include "packet_rewrite.h"
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static void put16(uint8_t *p, uint16_t value)
{
    p[0] = value >> 8;
    p[1] = value & 255;
}

static uint32_t sum_bytes(const uint8_t *p, size_t len, uint32_t sum)
{
    while (len >= 2) {
        sum += be16(p);
        p += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint16_t)p[0] << 8;
    }
    return sum;
}

static uint16_t finish_sum(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 65535) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(const uint8_t *data, size_t len)
{
    uint32_t sum = sum_bytes(data + 12, 8, 6 + len - 20);
    return finish_sum(sum_bytes(data + 20, len - 20, sum));
}

bool proxy_tcp_parse_local_header(const uint8_t *data, size_t available, size_t len,
                                  proxy_tcp_packet_t *packet)
{
    if (available < 40 || len < 40 || len > 65535 || data[0] != 0x45 || data[9] != 6 ||
        be16(data + 2) != len || (be16(data + 6) & 0xbfff) != 0 ||
        (data[32] >> 4) < 5 || (size_t)(data[32] >> 4) * 4 > len - 20 ||
        finish_sum(sum_bytes(data, 20, 0)) != 0) {
        return false;
    }
    memcpy(&packet->src_ip, data + 12, 4);
    memcpy(&packet->dst_ip, data + 16, 4);
    packet->src_port = be16(data + 20);
    packet->dst_port = be16(data + 22);
    packet->flags = data[33];
    return packet->src_port != 0 && packet->dst_port != 0;
}
bool proxy_tcp_parse(const uint8_t *data, size_t len, proxy_tcp_packet_t *packet)
{
    return proxy_tcp_parse_local_header(data,len,len,packet) && tcp_checksum(data,len)==0;
}

/* RFC 1624 field replacement: avoid rescanning payload after validated parsing. */
static uint16_t replace_word(uint16_t checksum, uint16_t old, uint16_t value)
{
    return finish_sum((uint32_t)(uint16_t)~checksum + (uint16_t)~old + value);
}
void proxy_tcp_rewrite_validated(uint8_t *data, bool source,
                                 uint32_t ip, uint16_t port, uint16_t peer_port)
{
    const size_t address=source?12:16, endpoint=source?20:22, peer=source?22:20;
    uint8_t ip_bytes[4]; memcpy(ip_bytes,&ip,4);
    uint16_t ip_sum=be16(data+10), tcp_sum=be16(data+36);
    for (unsigned n=0; n<4; n+=2) {
        uint16_t old=be16(data+address+n), value=be16(ip_bytes+n);
        ip_sum=replace_word(ip_sum,old,value); tcp_sum=replace_word(tcp_sum,old,value);
    }
    tcp_sum=replace_word(tcp_sum,be16(data+endpoint),port);
    if (peer_port) tcp_sum=replace_word(tcp_sum,be16(data+peer),peer_port);
    memcpy(data+address,ip_bytes,4); put16(data+endpoint,port);
    if (peer_port) put16(data+peer,peer_port);
    put16(data+10,ip_sum); put16(data+36,tcp_sum);
}
bool proxy_tcp_rewrite_peer(uint8_t *data, size_t len, bool source,
                             uint32_t ip, uint16_t port, uint16_t peer_port)
{
    proxy_tcp_packet_t packet;
    if (!port || !proxy_tcp_parse(data,len,&packet)) return false;
    proxy_tcp_rewrite_validated(data,source,ip,port,peer_port);
    return true;
}
bool proxy_tcp_rewrite(uint8_t *data, size_t len, bool source, uint32_t ip, uint16_t port)
{
    return proxy_tcp_rewrite_peer(data,len,source,ip,port,0);
}

bool proxy_udp_parse(const uint8_t *data, size_t len, proxy_udp_packet_t *packet)
{
    if (len < 28 || len > 65535 || data[0] != 0x45 || data[9] != 17 ||
        be16(data + 2) != len || (be16(data + 6) & 0xbfff) ||
        be16(data + 24) != len - 20 || finish_sum(sum_bytes(data,20,0)) != 0) return false;
    if (be16(data + 26) && finish_sum(sum_bytes(data+20,len-20,
        sum_bytes(data+12,8,17+len-20))) != 0) return false;
    memcpy(&packet->src_ip,data+12,4); memcpy(&packet->dst_ip,data+16,4);
    packet->src_port=be16(data+20); packet->dst_port=be16(data+22);
    packet->payload_length=len-28;
    return packet->src_port && packet->dst_port;
}
