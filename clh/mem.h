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
} CLH_Allocator;

#define clh_allocator_allocate(allocator, size) (allocator).allocate((allocator).data, size)
#define clh_allocator_free(allocator, ptr) (allocator).free((allocator).data, ptr)

void                *clh_system_allocator_allocate(void *, size_t size);
void                 clh_system_allocator_free(void *, void *data);
extern CLH_Allocator CLH_SYSTEM_ALLOCATOR;

/******************************************************************************/
/*                                dynamic pool                                */
/******************************************************************************/

struct CLH_DynMemPoolNode {
    struct CLH_DynMemPoolNode *prev;
    struct CLH_DynMemPoolNode *next;
    char                       data[];
};

typedef struct {
    struct CLH_DynMemPoolNode *used_list;
    struct CLH_DynMemPoolNode *free_list;
    size_t                     data_size;
    CLH_Allocator              backing_alloctor;
} CLH_DynMemPool;

struct CLH_DynMemPoolInitArgs {
    size_t        data_size;
    size_t        default_capacity;
    CLH_Allocator backing_alloctor;
};
void clh_dyn_mem_pool_init_(CLH_DynMemPool *pool, struct CLH_DynMemPoolInitArgs *args);
#define clh_dyn_mem_pool_init(pool, ...)                           \
    clh_dyn_mem_pool_init_(pool, &(struct CLH_DynMemPoolInitArgs){ \
                                     .backing_alloctor = CLH_SYSTEM_ALLOCATOR, __VA_ARGS__})

void          clh_dyn_mem_pool_destroy(CLH_DynMemPool *pool);
void         *clh_dyn_mem_pool_alloc(CLH_DynMemPool *pool);
void          clh_dyn_mem_pool_release(CLH_DynMemPool *pool, void *data);
CLH_Allocator clh_dyn_mem_pool_allocator(CLH_DynMemPool *pool);

#endif
