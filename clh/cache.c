#include "cache.h"
#include "log.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define CONF_USE_BUFFER_CACHE_NODE_ALLOCATOR

/******************************************************************************/
/*                           buffer cache allocator                           */
/******************************************************************************/

CLH_BufferCacheNodeAllocator buffer_cache_node_allocator_create(size_t block_size)
{
    CLH_BufferCacheNodeAllocatorBlock *block
        = malloc(sizeof(*block) + block_size * sizeof(*block->mem));
    block->next = NULL;
    block->count = 0;
    return (CLH_BufferCacheNodeAllocator){
        .first_block = block,
        .last_block = block,
        .block_size = block_size,
    };
}

CLH_BufferCacheNode *
buffer_cache_node_allocator_create_node(CLH_BufferCacheNodeAllocator *allocator)
{
    CLH_BufferCacheNode *node = NULL;

    if (allocator->last_block->count >= allocator->block_size) {
        CLH_BufferCacheNodeAllocatorBlock *block
            = malloc(sizeof(*block) + allocator->block_size * sizeof(*block->mem));
        block->next = NULL;
        block->count = 0;
        allocator->last_block->next = block;
        allocator->last_block = block;
    }
    node = &allocator->last_block->mem[allocator->last_block->count];
    allocator->last_block->count += 1;
    return node;
}

void buffer_cache_node_allocator_destroy(ucp_context_h                 ucp_ctx,
                                         CLH_BufferCacheNodeAllocator *allocator)
{
    CLH_BufferCacheNodeAllocatorBlock *cur = allocator->first_block;

    for (; cur != NULL;) {
        CLH_BufferCacheNodeAllocatorBlock *block = cur;

        cur = cur->next;
        for (size_t i = 0; i < block->count; ++i) {
            check_ucx(ucp_mem_unmap(ucp_ctx, block->mem[i].value.memh));
        }
        free(block);
    }
}

/******************************************************************************/
/*                                buffer cache                                */
/******************************************************************************/

CLH_BufferCache *clh_buffer_cache_create(ucp_context_h context, size_t)
{
    CLH_BufferCache *cache = malloc(sizeof(*cache));

    cache->context = context;
    cache->capacity = 0;
    cache->data = NULL;
    cache->size = 0;
    cache->mutex = clh_mutex_create();
    cache->allocator = buffer_cache_node_allocator_create(8192);
    return cache;
}

#ifndef CONF_USE_BUFFER_CACHE_NODE_ALLOCATOR
static bool free_tree_(CLH_BufferCache *cache, CLH_BufferCacheNode *node)
{
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
#endif

bool clh_buffer_cache_destroy(CLH_BufferCache *cache)
{
    clh_mutex_destroy(&cache->mutex);
#ifdef CONF_USE_BUFFER_CACHE_NODE_ALLOCATOR
    buffer_cache_node_allocator_destroy(cache->context, &cache->allocator);
#else
    free_tree_(cache, cache->data);
#endif
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
            assert(buffer.mem == (*cur)->value.mem);
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

    CLH_LOCK_REGION(cache->mutex)
    {
        CLH_BufferCacheNode **node = buffer_cache_search_(cache, buffer);

        if (*node == NULL) {
            ucp_mem_h            memh = NULL;
            ucp_mem_map_params_t params = {
                .field_mask = UCP_MEM_MAP_PARAM_FIELD_ADDRESS | UCP_MEM_MAP_PARAM_FIELD_LENGTH
                              | UCP_MEM_MAP_PARAM_FIELD_MEMORY_TYPE,
                .address = buffer.mem,
                .length = buffer.len,
                .memory_type = UCS_MEMORY_TYPE_UNKNOWN,
            };
            if (check_ucx(ucp_mem_map(cache->context, &params, &memh))) {
#ifdef CONF_USE_BUFFER_CACHE_NODE_ALLOCATOR
                *node = buffer_cache_node_allocator_create_node(&cache->allocator);
#else
                *node = malloc(sizeof(CLH_BufferCacheNode));
#endif
                (*node)->left = NULL;
                (*node)->right = NULL;
                (*node)->value.mem = buffer.mem;
                (*node)->value.memh = memh;
                result = (*node)->value;
                assert(result.mem == buffer.mem);
            }
        } else {
            result = (*node)->value;
            assert(result.mem == buffer.mem);
        }
    }
    assert(result.mem == buffer.mem);
    return result;
}

void remove_node_(CLH_BufferCacheNode **node)
{
    CLH_BufferCacheNode *left = (*node)->left;
    CLH_BufferCacheNode *right = (*node)->right;

#ifndef CONF_USE_BUFFER_CACHE_NODE_ALLOCATOR // TODO: it stays in the arena :/
    free(*node);
#endif
    *node = right;
    while (*node) {
        node = &(*node)->left;
    }
    *node = left;
}

bool clh_buffer_cache_unregister(CLH_BufferCache *cache, CLH_Buffer buffer)
{
    bool result = true;

    CLH_LOCK_REGION(cache->mutex)
    {
        CLH_BufferCacheNode **node = buffer_cache_search_(cache, buffer);

        if (*node == NULL) {
            result = false;
            CLH_EXIT_LOCK_REGION();
        }

        if (!check_ucx(ucp_mem_unmap(cache->context, (*node)->value.memh))) {
            result = false;
            CLH_EXIT_LOCK_REGION();
        }
        remove_node_(node);
    }
    return result;
}

bool clh_buffer_cache_is_registered(CLH_BufferCache *cache, CLH_Buffer buffer)
{
    CLH_BufferCacheNode **node = NULL;
    CLH_LOCK_REGION(cache->mutex) {
        node = buffer_cache_search_(cache, buffer);
    }
    clh_mutex_unlock(&cache->mutex);
    return *node != NULL;
}
