#include "mem.h"
#include <stdio.h>

/******************************************************************************/
/*                                 allocator                                  */
/******************************************************************************/

Allocator SYSTEM_ALLOCATOR = {
    .allocate = system_allocator_allocate,
    .free = system_allocator_free,
    .data = NULL,
};

void *system_allocator_allocate(void *, size_t size)
{
    return malloc(size);
}

void system_allocator_free(void *, void *data)
{
    free(data);
}

/******************************************************************************/
/*                                dynamic pool                                */
/******************************************************************************/

void dyn_mem_pool_init_(DynMemPool *pool, struct DynMemPoolInitArgs *args)
{
    if (args->data_size == 0) {
        fprintf(stderr, "error: dynamic pool requires a data size.\n");
    }
    pool->data_size = args->data_size;
    pool->backing_alloctor = args->backing_alloctor;
    pool->free_list = NULL;
    pool->used_list = NULL;

    struct DynMemPoolNode *node = NULL;
    for (size_t i = 0; i < args->default_capacity; ++i) {
        node = allocator_allocate(pool->backing_alloctor, 2 * 8 + pool->data_size);
        node->prev = NULL;
        node->next = pool->free_list;
        if (node->next != NULL) {
            node->next->prev = node;
        }
        pool->free_list = node;
    }
}

void dyn_mem_pool_destroy(DynMemPool *pool)
{
    struct DynMemPoolNode *cur, *next;

    for (cur = pool->free_list; cur != NULL; cur = next) {
        next = cur->next;
        allocator_free(pool->backing_alloctor, cur);
    }
    for (cur = pool->used_list; cur != NULL; cur = next) {
        next = cur->next;
        allocator_free(pool->backing_alloctor, cur);
    }
}

void *dyn_mem_pool_alloc(DynMemPool *pool)
{
    struct DynMemPoolNode *node = NULL;

    if (pool->free_list == NULL) {
        node = allocator_allocate(pool->backing_alloctor, 2 * 8 + pool->data_size);
        node->prev = NULL;
        node->next = NULL;
        pool->free_list = node;
    }
    node = pool->free_list;
    pool->free_list = node->next;
    if (node->next != NULL) {
        node->next->prev = NULL;
    }
    node->next = pool->used_list;
    if (node->next != NULL) {
        node->next->prev = node;
    }
    pool->used_list = node;
    return node->data;
}

void dyn_mem_pool_release(DynMemPool *pool, void *data)
{
    struct DynMemPoolNode *node = (struct DynMemPoolNode *)(data - 2 * 8);

    if (node->next != NULL) {
        node->next->prev = node->prev;
    }

    if (node->prev != NULL) {
        node->prev->next = node->next;
    } else {
        pool->used_list = node->next;
    }

    node->prev = NULL;
    node->next = pool->free_list;
    pool->free_list = node;
}

void *dyn_mem_pool_allocate_impl(void *pool, size_t)
{
    return dyn_mem_pool_alloc((DynMemPool*)pool);
}

void dyn_mem_pool_free_impl(void *pool, void *data)
{
    dyn_mem_pool_release((DynMemPool*)pool, data);
}

Allocator dyn_mem_pool_allocator(DynMemPool *pool)
{
    return (Allocator){
        .allocate = dyn_mem_pool_allocate_impl,
        .free = dyn_mem_pool_free_impl,
        .data = (void*)pool,
    };
}
