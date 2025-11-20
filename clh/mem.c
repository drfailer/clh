#include "mem.h"
#include <stdio.h>

/******************************************************************************/
/*                                 allocator                                  */
/******************************************************************************/

CLH_Allocator CLH_SYSTEM_ALLOCATOR = {
    .allocate = clh_system_allocator_allocate,
    .free = clh_system_allocator_free,
    .data = NULL,
};

void *clh_system_allocator_allocate(void *, size_t size)
{
    return malloc(size);
}

void clh_system_allocator_free(void *, void *data)
{
    free(data);
}

/******************************************************************************/
/*                                dynamic pool                                */
/******************************************************************************/

void clh_dyn_mem_pool_init_(CLH_DynMemPool *pool, struct CLH_DynMemPoolInitArgs *args)
{
    if (args->data_size == 0) {
        fprintf(stderr, "error: dynamic pool requires a data size.\n");
        return;
    }
    pool->data_size = args->data_size;
    pool->backing_alloctor = args->backing_alloctor;
    pool->free_list = NULL;
    pool->used_list = NULL;

    struct CLH_DynMemPoolNode *node = NULL;
    for (size_t i = 0; i < args->default_capacity; ++i) {
        node = clh_allocator_allocate(pool->backing_alloctor, sizeof(*node) + pool->data_size);
        node->prev = NULL;
        node->next = pool->free_list;
        pool->free_list = node;
    }
}

void clh_dyn_mem_pool_destroy(CLH_DynMemPool *pool)
{
    struct CLH_DynMemPoolNode *cur, *next;

    for (cur = pool->free_list; cur != NULL; cur = next) {
        next = cur->next;
        clh_allocator_free(pool->backing_alloctor, cur);
    }
    for (cur = pool->used_list; cur != NULL; cur = next) {
        next = cur->next;
        clh_allocator_free(pool->backing_alloctor, cur);
    }
}

void *clh_dyn_mem_pool_alloc(CLH_DynMemPool *pool)
{
    struct CLH_DynMemPoolNode *node = NULL;

    if (pool->free_list == NULL) {
        node = clh_allocator_allocate(pool->backing_alloctor, sizeof(*node) + pool->data_size);
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

void clh_dyn_mem_pool_release(CLH_DynMemPool *pool, void *data)
{
    struct CLH_DynMemPoolNode *node = (struct CLH_DynMemPoolNode *)(data - sizeof(*node));

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

void *clh_dyn_mem_pool_allocate_impl(void *pool, size_t)
{
    return clh_dyn_mem_pool_alloc((CLH_DynMemPool *)pool);
}

void clh_dyn_mem_pool_free_impl(void *pool, void *data)
{
    clh_dyn_mem_pool_release((CLH_DynMemPool *)pool, data);
}

CLH_Allocator clh_dyn_mem_pool_allocator(CLH_DynMemPool *pool)
{
    return (CLH_Allocator){
        .allocate = clh_dyn_mem_pool_allocate_impl,
        .free = clh_dyn_mem_pool_free_impl,
        .data = (void *)pool,
    };
}
