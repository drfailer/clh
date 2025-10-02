#include "cache.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>

CLH_BufferCache *clh_buffer_cache_create(ucp_context_h context, size_t)
{
    CLH_BufferCache *cache = malloc(sizeof(*cache));

    cache->context = context;
    cache->capacity = 0;
    cache->data = NULL;
    cache->size = 0;
    cache->mutex = clh_mutex_create();
    return cache;
}

bool free_tree_(CLH_BufferCache *cache, CLH_BufferCacheNode *node) {
    if (!node) {
        return true;
    }
    if (!free_tree_(cache, node->right)) {
        return false;
    }
    if (!free_tree_(cache, node->left)) {
        return false;
    }
    bool result = check_ucx(ucp_mem_unmap(cache->context, node->value.memh));
    free(node);
    return result;
}

bool clh_buffer_cache_destroy(CLH_BufferCache *cache)
{
    free_tree_(cache, cache->data);
    free(cache);
    return true;
}

static CLH_BufferCacheNode **buffer_cache_search_(CLH_BufferCache *cache, CLH_Buffer buffer)
{
    CLH_BufferCacheNode **cur = &cache->data;

    while (*cur) {
        if (buffer.mem < (*cur)->value.mem) {
            cur = &(*cur)->left;
        } else if (buffer.mem > (*cur)->value.mem) {
            cur = &(*cur)->right;
        } else {
            return cur;
        }
    }
    return cur;
}

CLH_BufferCacheEntry clh_buffer_cache_register_or_get(CLH_BufferCache *cache, CLH_Buffer buffer)
{
    CLH_BufferCacheEntry result = {NULL, NULL};
    if (buffer.mem == NULL) {
        return result;
    }

    clh_mutex_lock(&cache->mutex);
    CLH_BufferCacheNode **node = buffer_cache_search_(cache, buffer);

    if (*node == NULL) {
        ucp_mem_h memh = NULL;
        ucp_mem_map_params_t params = {
            .field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH
                          | UCP_MEM_MAP_PARAM_FIELD_MEMORY_TYPE,
            .address = buffer.mem,
            .length = buffer.len,
            .memory_type = UCS_MEMORY_TYPE_UNKNOWN,
        };
        if (check_ucx(ucp_mem_map(cache->context, &params, &memh))) {
            *node = malloc(sizeof(CLH_BufferCacheNode));
            (*node)->left = NULL;
            (*node)->right = NULL;
            (*node)->value.mem = buffer.mem;
            (*node)->value.memh = memh;
            result = (*node)->value;
        }
    } else {
        result = (*node)->value;
    }
    clh_mutex_unlock(&cache->mutex);
    return result;
}

void remove_node_(CLH_BufferCacheNode **node)
{
    CLH_BufferCacheNode *left = (*node)->left;
    CLH_BufferCacheNode *right = (*node)->right;

    free(*node);
    *node = right;
    while (*node) {
        node = &(*node)->left;
    }
    *node = left;
}

bool clh_buffer_cache_unregister(CLH_BufferCache *cache, CLH_Buffer buffer)
{
    bool result = true;

    clh_mutex_lock(&cache->mutex);
    CLH_BufferCacheNode **node = buffer_cache_search_(cache, buffer);

    if (*node == NULL) {
        result = false;
        goto exit;
    }

    if (!check_ucx(ucp_mem_unmap(cache->context, (*node)->value.memh))) {
        result = false;
        goto exit;
    }
    remove_node_(node);
    clh_mutex_unlock(&cache->mutex);
exit:
    return result;
}

bool clh_buffer_cache_is_registered(CLH_BufferCache *cache, CLH_Buffer buffer)
{
    clh_mutex_lock(&cache->mutex);
    CLH_BufferCacheNode **node = buffer_cache_search_(cache, buffer);
    clh_mutex_unlock(&cache->mutex);
    return *node != NULL;
}
