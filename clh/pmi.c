#include "pmi.h"
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <pmix.h>

struct CLH_PMI_Handle {
    clh_i32     node_id;
    clh_u32     nb_nodes;
    pmix_proc_t proc;
    pmix_proc_t proc_wild;
};

static bool check_pmi_(pmix_status_t status, char const *filename, size_t line)
{
    if (status == PMIX_SUCCESS) {
        return true;
    }
    fprintf(stderr, "%s:%ld\tCLH PMI ERROR\t%s\n", filename, line, PMIx_Error_string(status));
    return false;
}
#define check_pmi(status) check_pmi_(status, __FILE__, __LINE__)

CLH_PMI_Status clh_pmi_init(CLH_PMI_Handle *pmi)
{
    struct CLH_PMI_Handle *pmi_ = malloc(sizeof(*pmi_));

    *pmi = pmi_;
    if (!check_pmi(PMIx_Init(&pmi_->proc, NULL, 0))) {
        return CLH_PMI_STATUS_ERROR;
    }
    PMIX_LOAD_PROCID(&pmi_->proc_wild, pmi_->proc.nspace, PMIX_RANK_WILDCARD);
    pmi_->node_id = pmi_->proc.rank;

    pmix_value_t *value = NULL;
    if (!check_pmi(PMIx_Get(&pmi_->proc_wild, PMIX_JOB_SIZE, NULL, 0, &value))) {
        return CLH_PMI_STATUS_ERROR;
    }
    pmi_->nb_nodes = value->data.uint32;
    PMIX_VALUE_RELEASE(value);

    return CLH_PMI_STATUS_SUCCESS;
}

CLH_PMI_Status clh_pmi_finalize(CLH_PMI_Handle pmi)
{
    if (!check_pmi(PMIx_Finalize(NULL, 0))) {
        return CLH_PMI_STATUS_ERROR;
    }
    free(pmi);
    return CLH_PMI_STATUS_SUCCESS;
}

CLH_PMI_Status clh_pmi_put(CLH_PMI_Handle, char const *key, char *value, size_t size)
{
    pmix_value_t pmi_value = {
        .type = PMIX_BYTE_OBJECT,
        .data = {.bo = {.size = size, .bytes = value}},
    };

    if (!check_pmi(PMIx_Put(PMIX_GLOBAL, key, &pmi_value))) {
        return CLH_PMI_STATUS_ERROR;
    }
    return CLH_PMI_STATUS_SUCCESS;
}

CLH_PMI_Status clh_pmi_get(CLH_PMI_Handle pmi, clh_i32 node_id, char const *key, char *value,
                           size_t *size)
{
    struct CLH_PMI_Handle *pmi_ = (struct CLH_PMI_Handle*)pmi;
    pmix_value_t *pmi_value = NULL;
    pmix_proc_t   proc = {};

    PMIX_LOAD_PROCID(&proc, pmi_->proc.nspace, node_id);
    if (!check_pmi(PMIx_Get(&proc, key, NULL, 0, &pmi_value))) {
        return CLH_PMI_STATUS_ERROR;
    }
    memcpy(value, pmi_value->data.bo.bytes, pmi_value->data.bo.size);
    if (size) {
        *size = pmi_value->data.bo.size;
    }
    PMIX_VALUE_RELEASE(pmi_value);

    return CLH_PMI_STATUS_SUCCESS;
}

CLH_PMI_Status clh_pmi_fence(CLH_PMI_Handle pmi)
{
    struct CLH_PMI_Handle *pmi_ = (struct CLH_PMI_Handle*)pmi;
    pmix_info_t *info = NULL;
    bool         collect_data = false;

    PMIX_INFO_CREATE(info, 1);
    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &collect_data, PMIX_BOOL);
    if (!check_pmi(PMIx_Fence(&pmi_->proc_wild, 1, info, 1))) {
        return CLH_PMI_STATUS_ERROR;
    }
    PMIX_INFO_FREE(info, 1);

    return CLH_PMI_STATUS_SUCCESS;
}

CLH_PMI_Status clh_pmi_sync(CLH_PMI_Handle pmi)
{
    struct CLH_PMI_Handle *pmi_ = (struct CLH_PMI_Handle*)pmi;

    if (!check_pmi(PMIx_Commit())) {
        return CLH_PMI_STATUS_ERROR;
    }

    pmix_info_t *info = NULL;
    PMIX_INFO_CREATE(info, 1);
    bool collect_data = true;
    PMIX_INFO_LOAD(info, PMIX_COLLECT_DATA, &collect_data, PMIX_BOOL);
    if (!check_pmi(PMIx_Fence(&pmi_->proc_wild, 1, info, 1))) {
        return CLH_PMI_STATUS_ERROR;
    }
    PMIX_INFO_FREE(info, 1);

    return CLH_PMI_STATUS_SUCCESS;
}

char *clh_pmi_make_key(clh_i32 node_id)
{
    static char key[1024];
    sprintf(key, "CLH_AV_%d", node_id);
    return key;
}

char *clh_pmi_make_value(char *mem, size_t len)
{
    static char value[1024];
    assert(len < sizeof(value));
    memcpy(value, mem, len);
    value[len] = 0;
    return value;
}

clh_u32 clh_pmi_nb_nodes(CLH_PMI_Handle pmi)
{
    struct CLH_PMI_Handle *pmi_ = (struct CLH_PMI_Handle*)pmi;
    return pmi_->nb_nodes;
}

clh_i32 clh_pmi_node_id(CLH_PMI_Handle pmi)
{
    struct CLH_PMI_Handle *pmi_ = (struct CLH_PMI_Handle*)pmi;
    return pmi_->node_id;
}
