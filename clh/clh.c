#include "clh.h"
#include "pmi.h"
#include "ucx_utils.h"
#include <stdio.h>
#include <stdlib.h>

/******************************************************************************/
/*                                   status                                   */
/******************************************************************************/

char const *clh_status_string(CLH_Status status)
{
    static char result[64];

    switch (status) {
    case CLH_STATUS_SUCCESS:
        sprintf(result, "success");
        break;
    case CLH_STATUS_PMI_ERROR:
        sprintf(result, "pmi error");
        break;
    case CLH_STATUS_MEMORY_REGISTRATION_ERROR:
        sprintf(result, "memory registration error");
        break;
    case CLH_STATUS_REQUEST_FAILURE:
        sprintf(result, "request failure");
        break;
    case CLH_STATUS_ERROR:
        sprintf(result, "internal error");
        break;
    }
    return result;
}

/******************************************************************************/
/*                                  handlers                                  */
/******************************************************************************/

static void failure_handler(void *request, ucp_ep_h ep, ucs_status_t status)
{
    fprintf(stderr, "CLH failure handler called: %s {request = %p, ep = %p}\n", ucs_status_string(status), request, ep);
}

/******************************************************************************/
/*                           progressing the worker                           */
/******************************************************************************/

/*
 * This function is used in a thread and is in charge of progressing the workers.
 */
static void *progress(void *arg) {
    CLH_Handle handle = (CLH_Handle)arg;

    while (handle->run_progress) {
        clh_mutex_lock(&handle->mutex);
        while (ucp_worker_progress(handle->worker) > 0);
        clh_mutex_unlock(&handle->mutex);
        ucp_worker_wait(handle->worker);
    }
    return 0;
}

static void start_progress(CLH_Handle handle)
{
    handle->run_progress = true;
    handle->progress_thread = clh_thread_spawn(&progress, handle);
}

static void terminate_progress(CLH_Handle handle)
{
    handle->run_progress = false;
    ucp_worker_signal(handle->worker);
    clh_thread_join(handle->progress_thread);
}

CLH_Status clh_progress_signal(CLH_Handle handle)
{
    if (!check_ucx(ucp_worker_signal(handle->worker))){
        return CLH_STATUS_ERROR;
    }
    return CLH_STATUS_SUCCESS;
}

clh_u32 clh_progress_one(CLH_Handle handle)
{
    clh_u32 result = 0;

    clh_mutex_lock(&handle->mutex);
    result = ucp_worker_progress(handle->worker);
    clh_mutex_unlock(&handle->mutex);
    return result;
}

void clh_progress_all(CLH_Handle handle)
{
    clh_mutex_lock(&handle->mutex);
    while (ucp_worker_progress(handle->worker) > 0);
    clh_mutex_unlock(&handle->mutex);
}

/******************************************************************************/
/*                              init / finalize                               */
/******************************************************************************/

static inline CLH_Status init_ucp_context_(CLH_Handle handle)
{
    ucp_config_t *ucp_config;
    if (!check_ucx(ucp_config_read("MPI", NULL, &ucp_config))) {
        return CLH_STATUS_ERROR;
    }

    ucp_params_t ucp_params = {
        .field_mask = UCP_PARAM_FIELD_FEATURES | UCP_PARAM_FIELD_ESTIMATED_NUM_EPS,
        .features = UCP_FEATURE_TAG | UCP_FEATURE_WAKEUP | UCP_FEATURE_AM,
        .estimated_num_eps = clh_nb_nodes(handle),
    };
    if (!check_ucx(ucp_init(&ucp_params, ucp_config, &handle->ucp_context))) {
        ucp_config_release(ucp_config);
        return CLH_STATUS_ERROR;
    }
    ucp_config_release(ucp_config);
    return CLH_STATUS_SUCCESS;
}

static inline CLH_Status init_ucp_worker_(CLH_Handle handle)
{
    ucp_worker_params_t worker_params = {
        .field_mask = UCP_WORKER_PARAM_FIELD_THREAD_MODE,
        .thread_mode = UCS_THREAD_MODE_SERIALIZED,
    };
    if (!check_ucx(ucp_worker_create(handle->ucp_context, &worker_params, &handle->worker))) {
        return CLH_STATUS_ERROR;
    }
    return CLH_STATUS_SUCCESS;
}

static inline CLH_Status init_ucp_endpoints_(CLH_Handle handle)
{
    if (!check_ucx(
            ucp_worker_get_address(handle->worker, &handle->address.data, &handle->address.len))) {
        return CLH_STATUS_ERROR;
    }
    int this_node_id = clh_node_id(handle);
    size_t nb_nodes = clh_nb_nodes(handle);
    char *key = clh_pmi_make_key(this_node_id);
    char *value = clh_pmi_make_value((char *)handle->address.data, handle->address.len);
    clh_pmi_put(handle->pmi, key, value, handle->address.len);
    clh_pmi_sync(handle->pmi);

    handle->endpoints = malloc(nb_nodes * sizeof(ucp_ep_h));
    for (size_t node_id = 0; node_id < nb_nodes; ++node_id) {
        char peer_addr[1024] = {0};

        if (node_id == (size_t)this_node_id) {
            continue;
        }

        key = clh_pmi_make_key(node_id);
        clh_pmi_get(handle->pmi, node_id, key, peer_addr, NULL);

        ucp_ep_params_t ep_params = {
            .field_mask = UCP_EP_PARAM_FIELD_REMOTE_ADDRESS | UCP_EP_PARAM_FIELD_ERR_HANDLING_MODE
                          | UCP_EP_PARAM_FIELD_ERR_HANDLER,
            .address = (ucp_address_t *)peer_addr,
            .err_mode = UCP_ERR_HANDLING_MODE_PEER,
            .err_handler.cb = &failure_handler,
            .err_handler.arg = NULL,
        };
        if (!check_ucx(ucp_ep_create(handle->worker, &ep_params, &handle->endpoints[node_id]))) {
            return CLH_STATUS_ERROR;
        }
    }
    return CLH_STATUS_SUCCESS;
}

static inline CLH_Status init_cache_(CLH_Handle handle)
{
    handle->buffer_cache = clh_buffer_cache_create(handle->ucp_context, 8);
    if (handle->buffer_cache == NULL) {
        return CLH_STATUS_ERROR;
    }
    return CLH_STATUS_SUCCESS;
}

CLH_Status clh_init(CLH_Handle *handle)
{
    *handle = malloc(sizeof(struct CLH_HandleData));
    CLH_Status status = CLH_STATUS_SUCCESS;
    if (clh_pmi_init(&(*handle)->pmi) != CLH_PMI_STATUS_SUCCESS) {
        return CLH_STATUS_PMI_ERROR;
    }
    if ((status = init_ucp_context_(*handle)) != CLH_STATUS_SUCCESS) {
        return status;
    }
    if ((status = init_ucp_worker_(*handle)) != CLH_STATUS_SUCCESS) {
        return status;
    }
    if ((status = init_ucp_endpoints_(*handle)) != CLH_STATUS_SUCCESS) {
        return status;
    }
    if ((status = init_cache_(*handle)) != CLH_STATUS_SUCCESS) {
        return status;
    }
    (*handle)->mutex = clh_mutex_create();
    start_progress(*handle);
    return status;
}

CLH_Status clh_finalize(CLH_Handle handle)
{
    int this_node_id = clh_node_id(handle);
    size_t nb_nodes = clh_nb_nodes(handle);

    clh_pmi_sync(handle->pmi);
    if (!clh_buffer_cache_destroy(handle->buffer_cache)) {
        return CLH_STATUS_ERROR;
    }
    for (size_t node_id = 0; node_id < nb_nodes; ++node_id) {
        if (node_id == (size_t)this_node_id) {
            continue;
        }
        ucp_request_param_t params = {
            .op_attr_mask = UCP_OP_ATTR_FIELD_FLAGS,
            .flags = UCP_EP_CLOSE_FLAG_FORCE,
        };
        ucs_status_ptr_t status_ptr = ucp_ep_close_nbx(handle->endpoints[node_id], &params);
        if (UCS_PTR_IS_ERR(status_ptr)) {
            return CLH_STATUS_ERROR;
        }
        clh_wait(handle, &(CLH_Request){.status = status_ptr});
    }
    free(handle->endpoints);
    terminate_progress(handle);
    ucp_worker_release_address(handle->worker, handle->address.data);
    ucp_worker_destroy(handle->worker);
    ucp_cleanup(handle->ucp_context);
    clh_mutex_destroy(&handle->mutex);
    clh_pmi_finalize(handle->pmi);
    free(handle);
    return CLH_STATUS_SUCCESS;
}

// CLH_Status clh_warmup(CLH_Handle handle) {
//     // TODO
// }

/******************************************************************************/
/*                          communication functions                           */
/******************************************************************************/

CLH_Status clh_send(CLH_Handle handle, clh_u32 node_id, clh_u64 tag, CLH_Buffer buf,
                    CLH_Request *request)
{
    // TODO: idealy, we don't want to register small buffers, however, there is now way to query the RNDV_THRESH
    CLH_BufferCacheEntry bce = clh_buffer_cache_register_or_get(handle->buffer_cache, buf);
    if (bce.mem == NULL) {
        return CLH_STATUS_MEMORY_REGISTRATION_ERROR;
    }

    ucp_request_param_t params = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_MEMH | UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .memh = bce.memh,
    };
    clh_mutex_lock(&handle->mutex);
    request->status = ucp_tag_send_nbx(handle->endpoints[node_id], buf.mem, buf.len, tag, &params);
    clh_mutex_unlock(&handle->mutex);
    return CLH_STATUS_SUCCESS;
}

CLH_Status clh_recv(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Buffer buf,
                    CLH_Request *request)
{
    // TODO: idealy, we don't want to register small buffers, however, there is now way to query the RNDV_THRESH
    CLH_BufferCacheEntry bce = clh_buffer_cache_register_or_get(handle->buffer_cache, buf);
    if (bce.mem == NULL) {
        return CLH_STATUS_MEMORY_REGISTRATION_ERROR;
    }

    ucp_request_param_t params = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE | UCP_OP_ATTR_FIELD_MEMH | UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .datatype = ucp_dt_make_contig(1),
        .memh = bce.memh,
    };
    clh_mutex_lock(&handle->mutex);
    request->status = ucp_tag_recv_nbx(handle->worker, buf.mem, buf.len, tag, tag_mask, &params);
    clh_mutex_unlock(&handle->mutex);
    return UCS_PTR_IS_ERR(request->status) ? CLH_STATUS_REQUEST_FAILURE : CLH_STATUS_SUCCESS;
}

bool clh_probe(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Request *request)
{
    clh_mutex_lock(&handle->mutex);
    ucp_tag_message_h msg = ucp_tag_probe_nb(handle->worker, tag, tag_mask, 0, &request->tag_recv_info);
    clh_mutex_unlock(&handle->mutex);
    return msg != NULL;
}

/******************************************************************************/
/*                              active messages                               */
/******************************************************************************/

// ucs_status_t internal_am_handler(void *arg, const void *header, size_t header_length, void *data,
//                           size_t length, const ucp_am_recv_param_t *recv_param)
// {
//     CLH_AMHandlerData *hd = (CLH_AMHandlerData*)arg;
//
//     if (recv_param.recv_attr & UCP_AM_RECV_ATTR_FLAG_RNDV) {
//         CLH_BufferCacheEntry bce = clh_buffer_cache_register_or_get(handle->buffer_cache, buf);
//         if (bce.mem == NULL) {
//             return UCS_STATUS_ERROR;
//         }
//
//         ucp_request_param_t params = {
//             .op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE | UCP_OP_ATTR_FIELD_MEMH | UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
//             .datatype = ucp_dt_make_contig(1),
//             .memh = bce.memh,
//         };
//         ucs_status_ptr_t status = ucp_am_recv_data_nbx(handle->worker, data, buf.mem, buf.len, params);
//         clh_wait(handle, status); // TODO: how to do that efficiently?
//     } else {
//         CLH_Buffer header_buf = {header, header_length};
//         CLH_Buffer data_buf = {data, length};
//         hd->user_callback(hd->user_callback_args, header_buf, data_buf);
//     }
//     return UCS_OK;
// }
//
// CLH_Status clh_set_am_handler(CLH_Handle handle, clh_u32 id, CLH_AMHandler cb, void *args) {
//     CLH_Status result = CLH_STATUS_SUCCESS;
//
//     handlers_datas[id].handle = handle;
//     handlers_datas[id].user_callback = cb;
//     handlers_datas[id].user_callback_args = args;
//     ucp_am_handler_param_t params = {
//         .field_mask = UCP_AM_HANDLER_PARAM_FIELD_ID | UCP_AM_HANDLER_PARAM_FIELD_FLAGS | UCP_AM_HANDLER_PARAM_FIELD_CB | UCP_AM_HANDLER_PARAM_FIELD_ARG,
//         .id = id,
//         .flags = UCP_AM_FLAG_WHOLE_MSG, // or UCP_AM_FLAG_PERSISTENT_DATA?
//         .cb = internal_am_handler,
//         .arg = &handlers_datas[id],
//     };
//
//     clh_mutex_lock(&handle->mutex);
//     if (!check_ucx(ucp_worker_set_am_recv_handler(handle->worker, &params))) {
//         result = CLH_STATUS_ERROR;
//     }
//     clh_mutex_unlock(&handle->mutex);
//     return result;
// }
//
// CLH_Status clh_am_send(CLH_Handle handle, clh_u32 node_id, clh_u32 id, CLH_Buffer header,
//                        CLH_Buffer buf, CLH_Request *request)
// {
//     CLH_BufferCacheEntry bce = clh_buffer_cache_register_or_get(handle->buffer_cache, buf);
//     if (bce.mem == NULL) {
//         return CLH_STATUS_MEMORY_REGISTRATION_ERROR;
//     }
//
//     ucp_request_param_t params = {
//         .op_attr_mask = UCP_OP_ATTR_FIELD_DATATYPE | UCP_OP_ATTR_FIELD_MEMH | UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
//         .datatype = ucp_dt_make_contig(1),
//         .memh = bce.memh,
//     };
//
//     clh_mutex_lock(&handle->mutex);
//     request->status = ucp_am_send_nbx(handle->endpoints[node_id], id, header.mem, header.len,
//                                       buf.mem, buf.len, &params);
//     clh_mutex_unlock(&handle->mutex);
//     return CLH_STATUS_SUCCESS;
// }

/******************************************************************************/
/*                                  requests                                  */
/******************************************************************************/

CLH_Status clh_wait(CLH_Handle handle, CLH_Request *request)
{
    CLH_Status result = CLH_STATUS_SUCCESS;

    if (UCS_PTR_IS_ERR(request->status)) {
        return CLH_STATUS_REQUEST_FAILURE;
    }

    if (!UCS_PTR_IS_PTR(request->status)) {
        return CLH_STATUS_SUCCESS;
    }

    clh_mutex_lock(&handle->mutex);
    while (true) {
        ucs_status_t status = ucp_request_check_status(request->status);

        if (status == UCS_INPROGRESS) {
            ucp_worker_progress(handle->worker);
        } else if (status == UCS_OK) {
            break;
        } else {
            result = CLH_STATUS_REQUEST_FAILURE;
            goto unlock_and_return;
        }
    }
    clh_request_destroy(request);
unlock_and_return:
    clh_mutex_unlock(&handle->mutex);
    return result;
}

void clh_cancel(CLH_Handle handle, CLH_Request *request)
{
    clh_mutex_lock(&handle->mutex);
    ucp_request_cancel(handle->worker, request->status);
    clh_mutex_unlock(&handle->mutex);
}

bool clh_request_completed(CLH_Handle handle, CLH_Request const *request)
{
    clh_mutex_lock(&handle->mutex);
    ucs_status_t status = ucp_request_check_status(request->status);
    clh_mutex_unlock(&handle->mutex);
    return status != UCS_INPROGRESS;
}

CLH_Request clh_request_create()
{
    return (CLH_Request){
        .status = NULL,
        .tag_recv_info = {0},
    };
}

void clh_request_destroy(CLH_Request *request)
{
    if (request->status == NULL) {
        return;
    }
    ucp_request_free(request->status);
    request->status = NULL;
}

size_t clh_request_buffer_len(CLH_Request const *request)
{
    return request->tag_recv_info.length;
}

clh_u64 clh_request_tag(CLH_Request const *request)
{
    return request->tag_recv_info.sender_tag;
}

/******************************************************************************/
/*                                    pmi                                     */
/******************************************************************************/

void clh_barrier(CLH_Handle handle)
{
    clh_pmi_fence(handle->pmi);
}

clh_i32 clh_node_id(CLH_Handle handle)
{
    return clh_pmi_node_id(handle->pmi);
}

clh_u32 clh_nb_nodes(CLH_Handle handle)
{
    return clh_pmi_nb_nodes(handle->pmi);
}
