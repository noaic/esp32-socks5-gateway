#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
size_t socks_udp_encode(uint8_t *out, size_t capacity, uint32_t ipv4, uint16_t port,
                        const uint8_t *payload, size_t size);
bool socks_udp_decode(const uint8_t *data, size_t size, uint32_t *ipv4,
                      uint16_t *port, const uint8_t **payload, size_t *length);
