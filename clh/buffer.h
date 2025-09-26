#ifndef CLH_BUFFER
#define CLH_BUFFER
#include "clh_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ucp_byte *mem;
    size_t    len;
} CLH_Buffer;

typedef struct {
    CLH_Buffer *buffer;
    size_t      pos;
} CLH_BufferBuilder;

CLH_Buffer clh_buf_create(size_t len);

CLH_BufferBuilder clh_bb_create(CLH_Buffer *buffer);
bool              clh_bb_append(CLH_BufferBuilder *bb, ucp_byte *data, size_t count);

#ifdef __cplusplus
}
#endif

#endif // CLH_BUFFER
