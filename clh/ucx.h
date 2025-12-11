#ifndef CLH_UCX
#define CLH_UCX
#include "clh.h"

CLH_Status ucx_init(CLH_Handle handle);
CLH_Status ucx_finalize(CLH_Handle handle);
CLH_Status ucx_wait(CLH_Handle handle, ucs_status_ptr_t status_ptr);
ucs_status_ptr_t ucx_send(CLH_Handle handle, CLH_Request *request, ucp_mem_h memh);
ucs_status_ptr_t ucx_recv(CLH_Handle handle, CLH_Request *request, ucp_mem_h memh);

#endif
