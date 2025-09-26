#ifndef CLH_PMI
#define CLH_PMI
#include "clh_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLH_PMI_STATUS_SUCCESS,
    CLH_PMI_STATUS_ERROR,
} CLH_PMI_Status;

// using a void* allow including pmix.h only in the C file (simplify the usage of libclh)
typedef void *CLH_PMI_Handle;

CLH_PMI_Status clh_pmi_init(CLH_PMI_Handle *pmi);
CLH_PMI_Status clh_pmi_finalize(CLH_PMI_Handle pmi);

CLH_PMI_Status clh_pmi_put(CLH_PMI_Handle pmi, char const *key, char *value, size_t size);
CLH_PMI_Status clh_pmi_get(CLH_PMI_Handle pmi, clh_i32 node_id, char const *key, char *value,
                           size_t *size);

CLH_PMI_Status clh_pmi_fence(CLH_PMI_Handle pmi);
CLH_PMI_Status clh_pmi_sync(CLH_PMI_Handle pmi);

char *clh_pmi_make_key(clh_i32 node_id);
char *clh_pmi_make_value(char *mem, size_t len);

clh_u32 clh_pmi_nb_nodes(CLH_PMI_Handle pmi);
clh_i32 clh_pmi_node_id(CLH_PMI_Handle pmi);

#ifdef __cplusplus
}
#endif

#endif // CLH_PMI
