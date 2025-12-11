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

#endif
