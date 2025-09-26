#ifndef CLH_UCX_UTILS
#define CLH_UCX_UTILS
#include <ucp/api/ucp.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool check_ucx_(ucs_status_t status, char const *filename, size_t line);
#define check_ucx(status) check_ucx_(status, __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif // CLH_UCX_UTILS
