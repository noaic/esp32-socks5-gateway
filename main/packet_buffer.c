#include "packet_buffer.h"
#include <stddef.h>
#include <stdint.h>

struct pbuf *packet_buffer_prepare(struct pbuf *original)
{
    if (!original) return NULL;
    /* wlanif L2_TO_L3_COPY supplies this shape. Do not borrow shared, custom,
     * read-only, volatile driver or chained storage even if the header fits. */
    /* pbuf_init_alloced_pbuf stores only the low byte of pbuf_type. */
    if (original->type_internal == (uint8_t)PBUF_RAM && original->ref == 1 &&
        !(original->flags & PBUF_FLAG_IS_CUSTOM) && !original->next &&
        original->len == original->tot_len) return original;
    return pbuf_clone(PBUF_RAW, PBUF_RAM, original);
}

void packet_buffer_discard(struct pbuf *original, struct pbuf *prepared)
{
    if (prepared && prepared != original) pbuf_free(prepared);
}

void packet_buffer_commit(struct pbuf **original, struct pbuf *prepared)
{
    if (prepared && prepared != *original) {
        pbuf_free(*original);
        *original = prepared;
    }
}
