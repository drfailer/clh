#ifndef CLH_CACHE
#define CLH_CACHE
#include "buffer.h"
#include "clh_defs.h"
#include "thread.h"
#include <ucp/api/ucp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ucp_mem_h memh; // ucp memory handle
    ucp_byte *mem; // mem field of the buffer
} CLH_BufferCacheEntry;

typedef struct CLH_BufferCacheNode {
    CLH_BufferCacheEntry         value;
    struct CLH_BufferCacheNode *left;
    struct CLH_BufferCacheNode *right;
} CLH_BufferCacheNode;

// TODO: try using a binary tree and compare with the array
typedef struct {
    CLH_BufferCacheNode *data;
    size_t               size;
    size_t               capacity;
    ucp_context_h        context;
    CLH_Mutex            mutex;
} CLH_BufferCache;

CLH_BufferCache *clh_buffer_cache_create(ucp_context_h context, size_t capacity);
bool             clh_buffer_cache_destroy(CLH_BufferCache *cache);

CLH_BufferCacheEntry clh_buffer_cache_register_or_get(CLH_BufferCache *cache, CLH_Buffer buffer);
bool                 clh_buffer_cache_unregister(CLH_BufferCache *cache, CLH_Buffer buffer);
bool                 clh_buffer_cache_is_registered(CLH_BufferCache *cache, CLH_Buffer buffer);

#ifdef __cplusplus
}
#endif

#endif // CLH_CACHE
