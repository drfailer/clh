#ifndef CLH_MEM_POOL
#define CLH_MEM_POOL
#include <stdlib.h>

/******************************************************************************/
/*                                 allocator                                  */
/******************************************************************************/

typedef struct {
    void *(*allocate)(void *, size_t);
    void (*free)(void *, void *);
    void *data;
} Allocator;

#define allocator_allocate(allocator, size) (allocator).allocate((allocator).data, size)
#define allocator_free(allocator, ptr) (allocator).free((allocator).data, ptr)

void            *system_allocator_allocate(void *, size_t size);
void             system_allocator_free(void *, void *data);
extern Allocator SYSTEM_ALLOCATOR;

/******************************************************************************/
/*                                dynamic pool                                */
/******************************************************************************/

struct DynMemPoolNode {
    struct DynMemPoolNode *prev;
    struct DynMemPoolNode *next;
    char                   data[];
};

typedef struct {
    struct DynMemPoolNode *used_list;
    struct DynMemPoolNode *free_list;
    size_t                 data_size;
    Allocator              backing_alloctor;
} DynMemPool;

struct DynMemPoolInitArgs {
    size_t    data_size;
    size_t    default_capacity;
    Allocator backing_alloctor;
};
void dyn_mem_pool_init_(DynMemPool *pool, struct DynMemPoolInitArgs *args);
#define dyn_mem_pool_init(pool, ...) \
    dyn_mem_pool_init_(              \
        pool, &(struct DynMemPoolInitArgs){.backing_alloctor = SYSTEM_ALLOCATOR, __VA_ARGS__})

void      dyn_mem_pool_destroy(DynMemPool *pool);
void     *dyn_mem_pool_alloc(DynMemPool *pool);
void      dyn_mem_pool_release(DynMemPool *pool, void *data);
Allocator dyn_mem_pool_allocator(DynMemPool *pool);

#endif
