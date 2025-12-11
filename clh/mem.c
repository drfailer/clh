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
