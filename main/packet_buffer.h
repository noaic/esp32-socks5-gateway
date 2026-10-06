#pragma once
#include "lwip/pbuf.h"

/* Prepare private contiguous storage; may borrow the exclusively owned input.
 * Until commit, the original remains owned by the IPv4 caller on every error. */
struct pbuf *packet_buffer_prepare(struct pbuf *original);
void packet_buffer_discard(struct pbuf *original, struct pbuf *prepared);
void packet_buffer_commit(struct pbuf **original, struct pbuf *prepared);
