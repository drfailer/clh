#include "buffer.h"
#include <stdlib.h>
#include <string.h>

CLH_Buffer clh_buf_create(size_t len)
{
    return (CLH_Buffer){
        .mem = malloc(len),
        .len = len,
    };
}

CLH_BufferBuilder clh_bb_create(CLH_Buffer *buffer)
{
    return (CLH_BufferBuilder){
        .buffer = buffer,
        .pos = 0,
    };
}

bool clh_bb_append(CLH_BufferBuilder *bb, ucp_byte *data, size_t count)
{
    if (bb->pos + count >= bb->buffer->len) {
        return false;
    }
    memcpy(&bb->buffer[bb->pos], data, count);
    bb->pos += count;
    return true;
}
