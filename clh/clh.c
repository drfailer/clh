#include "clh.h"
#include "log.h"
#include "pmi.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CONF_WORKER_WAIT
#define CONF_LOOP_SLEEP_TIME 1000
// #define CONF_PROGRESS_OP_COUNT
// #define CONF_PROFILE
#define CONF_USE_SEND_REQUEST_QUEUE
#define CONF_USE_RECV_REQUEST_QUEUE
#define CONF_USE_PROBE_REQUEST_QUEUE

#ifdef CONF_PROFILE
typedef struct {
    bool          done;
    CLH_TimePoint tp_start;
} CLH_PerfRegionData;

inline CLH_PerfRegionData clh_perf_region_data_start()
{
    return (CLH_PerfRegionData){false, clh_perf_tp_get()};
}

#define CLH_PERF_REGION(handle_, category_, name_)                               \
    for (CLH_PerfRegionData rd_ = clh_perf_region_data_start(); !rd_.done;       \
         clh_mutex_lock(&handle_->mutex),                                        \
                            handle_->stats.category_.name_##_dur                 \
                            += clh_perf_tp_dur(rd_.tp_start, clh_perf_tp_get()), \
                            handle_->stats.category_.name_##_count += 1,         \
                            clh_mutex_unlock(&handle_->mutex), rd_.done = true)
#else
#define CLH_PERF_REGION(handle_, category_, name_)
#endif

#ifdef CONF_WORKER_WAIT
#define WORKER_WAIT(handle)                  \
    do {                                     \
        if (queues_emtpy_(handle)) {         \
            ucp_worker_wait(handle->worker); \
        }                                    \
    } while (false);
#define WORKER_SIGNAL(handle) ucp_worker_signal(handle->worker)
#else
#define WORKER_WAIT(handle)
#define WORKER_SIGNAL(handle)
#endif

#ifdef CONF_LOOP_SLEEP_TIME
#define SLEEP() usleep(CONF_LOOP_SLEEP_TIME)
#else
#define SLEEP()
#endif

#ifdef CONF_PROGRESS_OP_COUNT
// TODO: we need an op counter in the handle
#define WORKER_PROGRESS(handle)                          \
    for (size_t i = 0; i < handle->ops_queue.len; ++i) { \
        ucp_worker_progress(handle->worker);             \
    }
#else
#define WORKER_PROGRESS(handle)                     \
    while (ucp_worker_progress(handle->worker) > 0) \
        ;
#endif

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
    fprintf(stderr, "CLH failure handler called: %s {request = %p, ep = %p}\n",
            ucs_status_string(status), request, ep);
}

/******************************************************************************/
/*                            ucx helper functions                            */
/******************************************************************************/

static CLH_Status ucx_wait(CLH_Handle handle, ucs_status_ptr_t status_ptr)
{
    if (UCS_PTR_IS_ERR(status_ptr)) {
        return CLH_STATUS_REQUEST_FAILURE;
    }

    if (!UCS_PTR_IS_PTR(status_ptr)) {
        return CLH_STATUS_SUCCESS;
    }

    while (true) {
        ucs_status_t status = ucp_request_check_status(status_ptr);

        if (status == UCS_INPROGRESS) {
            ucp_worker_progress(handle->worker);
        } else if (status == UCS_OK) {
            break;
        } else {
            return CLH_STATUS_REQUEST_FAILURE;
        }
    }
    ucp_request_free(status_ptr);
    return CLH_STATUS_SUCCESS;
}

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
#if (defined(CONF_USE_SEND_REQUEST_QUEUE) && defined(CONF_USE_RECV_REQUEST_QUEUE) \
     && defined(CONF_USE_PROBE_REQUEST_QUEUE))
        .thread_mode = UCS_THREAD_MODE_SINGLE,
#else
        .thread_mode = UCS_THREAD_MODE_MULTI,
#endif
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
    int    this_node_id = clh_node_id(handle);
    size_t nb_nodes = clh_nb_nodes(handle);
    char  *key = clh_pmi_make_key(this_node_id);
    char  *value = clh_pmi_make_value((char *)handle->address.data, handle->address.len);
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

static CLH_Status ucx_init(CLH_Handle handle)
{
    CLH_Status status = CLH_STATUS_SUCCESS;
    if ((status = init_ucp_context_(handle)) != CLH_STATUS_SUCCESS) {
        fprintf(stderr, "error: init ucp context.\n");
        return status;
    }
    if ((status = init_ucp_worker_(handle)) != CLH_STATUS_SUCCESS) {
        fprintf(stderr, "error: init ucp worker.\n");
        return status;
    }
    if ((status = init_ucp_endpoints_(handle)) != CLH_STATUS_SUCCESS) {
        fprintf(stderr, "error: init ucp endpoints.\n");
        return status;
    }
    if ((status = init_cache_(handle)) != CLH_STATUS_SUCCESS) {
        fprintf(stderr, "error: init cache.\n");
        return status;
    }
    return status;
}

static CLH_Status ucx_finalize(CLH_Handle handle)
{
    int    this_node_id = clh_node_id(handle);
    size_t nb_nodes = clh_nb_nodes(handle);

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
        ucx_wait(handle, status_ptr);
    }
    free(handle->endpoints);
    ucp_worker_release_address(handle->worker, handle->address.data);
    ucp_worker_destroy(handle->worker);
    ucp_cleanup(handle->ucp_context);
    return CLH_STATUS_SUCCESS;
}

#define complete_request_(request, ...)                    \
    do {                                                   \
        clh_mutex_lock(&request->mutex);                   \
        {__VA_ARGS__} request->completed = true;           \
        clh_conditional_variable_notify_one(&request->cv); \
        clh_mutex_unlock(&request->mutex);                 \
    } while (false);

static ucs_status_ptr_t ucx_send(CLH_Handle handle, CLH_Request *request, ucp_mem_h memh)
{
    ucp_request_param_t params = {
        .op_attr_mask = UCP_OP_ATTR_FIELD_MEMH | UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .memh = memh,
    };
    return ucp_tag_send_nbx(handle->endpoints[request->data.send.dest],
                            request->data.send.buffer.mem, request->data.send.buffer.len,
                            request->data.send.tag, &params);
}

static ucs_status_ptr_t ucx_recv(CLH_Handle handle, CLH_Request *request, ucp_mem_h memh)
{
    ucp_request_param_t params = {
        .op_attr_mask
        = UCP_OP_ATTR_FIELD_DATATYPE | UCP_OP_ATTR_FIELD_MEMH | UCP_OP_ATTR_FLAG_NO_IMM_CMPL,
        .datatype = ucp_dt_make_contig(1),
        .memh = memh,
    };
    ucs_status_ptr_t status;

    if (request->data.recv.msg != NULL) {
        status
            = ucp_tag_msg_recv_nbx(handle->worker, request->data.recv.buffer.mem,
                                   request->data.recv.buffer.len, request->data.recv.msg, &params);
    } else {
        status = ucp_tag_recv_nbx(handle->worker, request->data.recv.buffer.mem,
                                  request->data.recv.buffer.len, request->data.recv.tag,
                                  request->data.recv.tag_mask, &params);
    }
    return status;
}

static bool validate_status_ptr_(CLH_Handle handle, CLH_Op *op)
{
    if (UCS_PTR_IS_ERR(op->status_ptr)) {
        return false;
    }
    if (!UCS_PTR_IS_PTR(op->status_ptr)) {
        complete_request_(op->request);
    } else {
#if (defined(CONF_USE_SEND_REQUEST_QUEUE) && defined(CONF_USE_RECV_REQUEST_QUEUE))
        array_append(handle->ops_queue, *op);
#else
        CLH_LOCK_REGION(handle->mutex)
        {
            array_append(handle->ops_queue, *op);
        }
#endif
    }
    return true;
}

/******************************************************************************/
/*                                    run                                     */
/******************************************************************************/

#define PROCESS_SEND_RECV_QUEUE_FUNC(func_name, queue_id, ucx_func)                           \
    static CLH_Status func_name(CLH_Handle handle)                                            \
    {                                                                                         \
        CLH_Status           status = CLH_STATUS_SUCCESS;                                     \
        CLH_RequestArray    *queue;                                                           \
        CLH_BufferCacheEntry bce;                                                             \
                                                                                              \
        if (handle->request_queues[queue_id].requests.len == 0) {                             \
            return status;                                                                    \
        }                                                                                     \
                                                                                              \
        CLH_LOCK_REGION(handle->request_queues[queue_id].mutex)                               \
        {                                                                                     \
            queue = &handle->request_queues[queue_id].requests;                               \
            for (size_t i = 0; i < queue->len; ++i) {                                         \
                CLH_Op op = {.request = queue->ptr[i], .status_ptr = NULL};                   \
                CLH_PERF_REGION(handle, cache, register)                                      \
                {                                                                             \
                    TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")              \
                    {                                                                         \
                        bce = clh_buffer_cache_register_or_get(handle->buffer_cache,          \
                                                               op.request->data.send.buffer); \
                        if (bce.mem != op.request->data.send.buffer.mem) {                    \
                            clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,         \
                                     op.request->data.send.buffer.mem);                       \
                            clh_error("UCX", "%s", "registration error (send).");             \
                            status = CLH_STATUS_MEMORY_REGISTRATION_ERROR;                    \
                            CLH_EXIT_LOCK_REGION();                                           \
                        }                                                                     \
                    }                                                                         \
                }                                                                             \
                op.status_ptr = ucx_func(handle, op.request, bce.memh);                       \
                if (!validate_status_ptr_(handle, &op)) {                                     \
                    clh_error("UCX", "%s", #ucx_func " request failure (send).");             \
                    status = CLH_STATUS_REQUEST_FAILURE;                                      \
                    CLH_EXIT_LOCK_REGION();                                                   \
                }                                                                             \
            }                                                                                 \
            queue->len = 0;                                                                   \
        }                                                                                     \
        return status;                                                                        \
    }
PROCESS_SEND_RECV_QUEUE_FUNC(process_send_queue_, CLH_REQUEST_TYPE_SEND, ucx_send)
PROCESS_SEND_RECV_QUEUE_FUNC(process_recv_queue_, CLH_REQUEST_TYPE_RECV, ucx_recv)
#undef PROCESS_SEND_RECV_QUEUE_FUNC

static CLH_Status process_probe_queue_(CLH_Handle handle)
{
    CLH_Status        status = CLH_STATUS_SUCCESS;
    CLH_RequestArray *queue;

    if (handle->request_queues[CLH_REQUEST_TYPE_PROBE].requests.len == 0) {
        return status;
    }

    CLH_LOCK_REGION(handle->request_queues[CLH_REQUEST_TYPE_PROBE].mutex)
    {
        queue = &handle->request_queues[CLH_REQUEST_TYPE_PROBE].requests;
        for (size_t i = 0; i < queue->len; ++i) {
            CLH_Request        *request = queue->ptr[i];
            ucp_tag_recv_info_t infos;
            ucp_tag_message_h   msg;

            msg = ucp_tag_probe_nb(handle->worker, request->data.probe.tag,
                                   request->data.probe.tag_mask, request->data.probe.remove,
                                   &infos);
            complete_request_(request, {
                request->data.probe.result = (msg != NULL);
                request->data.probe.buffer_len = infos.length;
                request->data.probe.sender_tag = infos.sender_tag;
                request->data.probe.msg = msg;
            });
        }
        queue->len = 0;
    }
    return status;
}

static CLH_Status process_ops_queue_(CLH_Handle handle)
{
    CLH_Ops *queue = &handle->ops_queue;
    size_t   idx = 0;

    while (idx < queue->len) {
        CLH_Op *op = &queue->ptr[idx];

        assert(UCS_PTR_IS_PTR(op->status_ptr) && !UCS_PTR_IS_ERR(op->status_ptr));
        ucs_status_t status = ucp_request_check_status(op->status_ptr);

        if (status == UCS_INPROGRESS) {
            idx += 1;
        } else if (status == UCS_OK) {
            CLH_Request *request = op->request;
            ucp_request_free(op->status_ptr);
            array_remove(handle->ops_queue, idx);
            complete_request_(request);
        } else {
            clh_error("UCX", "%s", "request error.");
            array_remove(handle->ops_queue, idx);
            return CLH_STATUS_ERROR;
        }
    }
    return CLH_STATUS_SUCCESS;
}

static inline CLH_Status process_shared_queues_(CLH_Handle handle)
{
#if defined(CONF_USE_PROBE_REQUEST_QUEUE)
    assert(process_probe_queue_(handle) == CLH_STATUS_SUCCESS);
#endif
#if defined(CONF_USE_SEND_REQUEST_QUEUE)
    assert(process_send_queue_(handle) == CLH_STATUS_SUCCESS);
#endif
#if defined(CONF_USE_RECV_REQUEST_QUEUE)
    assert(process_recv_queue_(handle) == CLH_STATUS_SUCCESS);
#endif
    return CLH_STATUS_SUCCESS;
}

static bool queues_emtpy_(CLH_Handle handle)
{
    return handle->ops_queue.len == 0
#if defined(CONF_USE_SEND_REQUEST_QUEUE)
           && handle->request_queues[CLH_REQUEST_TYPE_SEND].requests.len == 0
#endif
#if defined(CONF_USE_RECV_REQUEST_QUEUE)
           && handle->request_queues[CLH_REQUEST_TYPE_RECV].requests.len == 0
#endif
#if defined(CONF_USE_PROBE_REQUEST_QUEUE)
           && handle->request_queues[CLH_REQUEST_TYPE_PROBE].requests.len == 0
#endif
        ;
}

static void *run_(void *arg)
{
    CLH_Handle handle = (CLH_Handle)arg;

    assert(ucx_init(handle) == CLH_STATUS_SUCCESS);

    handle->run = true; // make sure ucx is inited
    clh_conditional_variable_notify_one(&handle->init_cv);

    while (handle->run || !queues_emtpy_(handle)) {
        WORKER_WAIT(handle);
        CLH_PERF_REGION(handle, run, process_shared_queues)
        {
            TRACER_LOCAL_REGION(handle->tracer, "process shared queues,#F7C48BFF", "clh.worker")
            {
                process_shared_queues_(handle);
            }
        }
        CLH_PERF_REGION(handle, run, progress)
        {
            TRACER_LOCAL_REGION(handle->tracer, "progress worker,#CC0000FF", "clh.worker")
            {
                WORKER_PROGRESS(handle);
            }
        }
        CLH_PERF_REGION(handle, run, process_ops)
        {
            TRACER_LOCAL_REGION(handle->tracer, "process ops queue,#009BC2FF", "clh.worker")
            {
                assert(process_ops_queue_(handle) == CLH_STATUS_SUCCESS);
            }
        }
        SLEEP();
    }
    assert(ucx_finalize(handle) == CLH_STATUS_SUCCESS);
    printf("CLH RUN TERMINATE\n");
    return 0;
}

static void start_(CLH_Handle handle)
{
    array_create(handle->request_queues[CLH_REQUEST_TYPE_SEND].requests, 128);
    array_create(handle->request_queues[CLH_REQUEST_TYPE_RECV].requests, 128);
    array_create(handle->request_queues[CLH_REQUEST_TYPE_PROBE].requests, 128);
    array_create(handle->ops_queue, 128);
    handle->request_queues[CLH_REQUEST_TYPE_SEND].mutex = clh_mutex_create();
    handle->request_queues[CLH_REQUEST_TYPE_RECV].mutex = clh_mutex_create();
    handle->request_queues[CLH_REQUEST_TYPE_PROBE].mutex = clh_mutex_create();

    handle->run = false;
    CLH_Mutex init_mutex = clh_mutex_create();
    handle->init_cv = clh_conditional_variable_create();
    handle->run_thread = clh_thread_spawn(&run_, handle);
    clh_conditional_variable_wait(&handle->init_cv, &init_mutex);
    clh_conditional_variable_destroy(&handle->init_cv);
    clh_mutex_destroy(&init_mutex);
}

static void terminate_(CLH_Handle handle)
{
    printf("CLH TERMINATE %d\n", clh_node_id(handle));
    handle->run = false;
    WORKER_SIGNAL(handle);
    clh_thread_join(handle->run_thread);
    array_destroy(handle->request_queues[CLH_REQUEST_TYPE_SEND].requests);
    array_destroy(handle->request_queues[CLH_REQUEST_TYPE_RECV].requests);
    array_destroy(handle->request_queues[CLH_REQUEST_TYPE_PROBE].requests);
    array_destroy(handle->ops_queue);
    clh_mutex_destroy(&handle->request_queues[CLH_REQUEST_TYPE_SEND].mutex);
    clh_mutex_destroy(&handle->request_queues[CLH_REQUEST_TYPE_RECV].mutex);
    clh_mutex_destroy(&handle->request_queues[CLH_REQUEST_TYPE_PROBE].mutex);

    // clang-format off
#ifdef CONF_PROFILE
    printf("==================== CLH STATS ====================\n");
    printf("progress: %.2f ms (count = %ld)\n", handle->stats.run.progress_dur, handle->stats.run.progress_count);
    printf("process shared queues: %.2f ms (count = %ld)\n", handle->stats.run.process_shared_queues_dur, handle->stats.run.process_shared_queues_count);
    printf("process request queue: %.2f ms (count = %ld)\n", handle->stats.run.process_requests_dur, handle->stats.run.process_requests_count);
    printf("register: %.2f ms (count = %ld)\n", handle->stats.cache.register_dur, handle->stats.cache.register_count);
    printf("send: %.2f ms (count = %ld)\n", handle->stats.comm.send_dur, handle->stats.comm.send_count);
    printf("recv: %.2f ms (count = %ld)\n", handle->stats.comm.recv_dur, handle->stats.comm.recv_count);
    printf("probe: %.2f ms (count = %ld)\n", handle->stats.comm.probe_dur, handle->stats.comm.probe_count);
    printf("probe wait: %.2f ms (count = %ld)\n", handle->stats.comm.probe_wait_dur, handle->stats.comm.probe_wait_count);
    printf("===================================================\n");
#endif
    // clang-format on
}

/******************************************************************************/
/*                              init / finalize                               */
/******************************************************************************/

CLH_Status clh_init(CLH_Handle *handle)
{
    *handle = calloc(1, sizeof(struct CLH_HandleData));
    (*handle)->tracer = TRACER_CREATE(0);
    TRACER_LOCAL_REGION((*handle)->tracer, "init", "clh")
    {
        if (clh_pmi_init(&(*handle)->pmi) != CLH_PMI_STATUS_SUCCESS) {
            return CLH_STATUS_PMI_ERROR;
        }
        (*handle)->mutex = clh_mutex_create();
        (*handle)->request_pool.mutex = clh_mutex_create();
        (*handle)->request_pool = (CLH_RequestPool){{}, NULL, NULL}; // prealloc???
        start_(*handle);
    }
    return CLH_STATUS_SUCCESS;
}

CLH_Status clh_finalize(CLH_Handle handle)
{
    TRACER_LOCAL_REGION(handle->tracer, "finalize", "clh")
    {
        clh_pmi_sync(handle->pmi);
        terminate_(handle);
        clh_pmi_finalize(handle->pmi);
        clh_mutex_destroy(&handle->mutex);
        clh_mutex_destroy(&handle->request_pool.mutex);
        for (struct CLH_RequestPoolNode *node = handle->request_pool.free_nodes; node != NULL;) {
            struct CLH_RequestPoolNode *next = node->next;
            clh_request_pool_node_destroy(node);
            node = next;
        }
        TRACER_WRITE(handle->tracer, "clh_trace.tr");
    }
    TRACER_DESTROY(handle->tracer);
    free(handle);
    return CLH_STATUS_SUCCESS;
}

// CLH_Status clh_warmup(CLH_Handle handle) {
//     // TODO
// }

/******************************************************************************/
/*                          communication functions                           */
/******************************************************************************/

// send ////////////////////////////////////////////////////////////////////////

#ifdef CONF_USE_SEND_REQUEST_QUEUE
CLH_Request *clh_send(CLH_Handle handle, clh_u32 dest, clh_u64 tag, CLH_Buffer buffer)
{
    CLH_Request *request = clh_request_get(handle);
    request->type = CLH_REQUEST_TYPE_SEND;
    request->completed = false;
    request->data.send.buffer = buffer;
    request->data.send.tag = tag;
    request->data.send.dest = dest;
    CLH_PERF_REGION(handle, comm, send)
    {
        TRACER_LOCAL_REGION(handle->tracer, "send,#00990CFF", "clh.op")
        {
            CLH_LOCK_REGION(handle->request_queues[CLH_REQUEST_TYPE_SEND].mutex)
            {
                array_append(handle->request_queues[CLH_REQUEST_TYPE_SEND].requests, request);
            }
            WORKER_SIGNAL(handle);
        }
    }
    return request;
}
#else // CONF_USE_SEND_REQUEST_QUEUE
CLH_Request *clh_send(CLH_Handle handle, clh_u32 dest, clh_u64 tag, CLH_Buffer buffer)
{
    CLH_BufferCacheEntry bce;
    CLH_Request         *request = clh_request_get(handle);
    request->type = CLH_REQUEST_TYPE_SEND;
    request->completed = false;
    request->data.send.buffer = buffer;
    request->data.send.tag = tag;
    request->data.send.dest = dest;
    CLH_PERF_REGION(handle, comm, send)
    {
        TRACER_LOCAL_REGION(handle->tracer, "send,#00990CFF", "clh.op")
        {
            CLH_Op op = {.request = request, .status_ptr = NULL};
            // CLH_PERF_REGION(handle, cache, register)
            TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")
            {
                bce = clh_buffer_cache_register_or_get(handle->buffer_cache,
                                                       op.request->data.send.buffer);
                if (bce.mem != op.request->data.send.buffer.mem) {
                    clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,
                             op.request->data.send.buffer.mem);
                    clh_error("UCX", "%s", "registration error (send).");
                    clh_request_release(handle, request);
                    return NULL;
                }
            }
            op.status_ptr = ucx_send(handle, op.request, bce.memh);
            if (!validate_status_ptr_(handle, &op)) {
                clh_error("UCX", "%s", "ucx_send request failure (send).");
                clh_request_release(handle, request);
                return NULL;
            }
            WORKER_SIGNAL(handle);
        }
    }
    return request;
}
#endif // CONF_USE_SEND_REQUEST_QUEUE

// recv ////////////////////////////////////////////////////////////////////////

#ifdef CONF_USE_RECV_REQUEST_QUEUE
CLH_Request *clh_recv(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Buffer buffer)
{
    CLH_Request *request = clh_request_get(handle);
    request->type = CLH_REQUEST_TYPE_RECV;
    request->completed = false;
    request->data.recv.buffer = buffer;
    request->data.recv.tag = tag;
    request->data.recv.tag_mask = tag_mask;
    request->data.recv.msg = NULL;
    CLH_PERF_REGION(handle, comm, recv)
    {
        TRACER_LOCAL_REGION(handle->tracer, "recv,#F5E900FF", "clh.op")
        {
            CLH_LOCK_REGION(handle->request_queues[CLH_REQUEST_TYPE_RECV].mutex)
            {
                array_append(handle->request_queues[CLH_REQUEST_TYPE_RECV].requests, request);
            }
            WORKER_SIGNAL(handle);
        }
    }
    return request;
}

CLH_Request *clh_request_recv(CLH_Handle handle, CLH_Request *request, CLH_Buffer buffer)
{
    clh_u64           tag = request->data.probe.sender_tag;
    bool              remove = request->data.probe.remove;
    ucp_tag_message_h msg = request->data.probe.msg;

    request->type = CLH_REQUEST_TYPE_RECV;
    request->completed = false;
    request->data.recv.buffer = buffer;
    request->data.recv.tag = tag;
    request->data.recv.tag_mask = 0xFFFFFFFFFFFFFFFF;
    request->data.recv.msg = remove ? msg : NULL;
    CLH_PERF_REGION(handle, comm, recv)
    {
        TRACER_LOCAL_REGION(handle->tracer, "request recv,#F5E900FF", "clh.op")
        {
            CLH_LOCK_REGION(handle->request_queues[CLH_REQUEST_TYPE_RECV].mutex)
            {
                array_append(handle->request_queues[CLH_REQUEST_TYPE_RECV].requests, request);
            }
            WORKER_SIGNAL(handle);
        }
    }
    return request;
}
#else // CONF_USE_RECV_REQUEST_QUEUE
CLH_Request *clh_recv(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Buffer buffer)
{
    CLH_BufferCacheEntry bce;
    CLH_Request         *request = clh_request_get(handle);
    request->type = CLH_REQUEST_TYPE_RECV;
    request->completed = false;
    request->data.recv.buffer = buffer;
    request->data.recv.tag = tag;
    request->data.recv.tag_mask = tag_mask;
    request->data.recv.msg = NULL;
    CLH_PERF_REGION(handle, comm, recv)
    {
        TRACER_LOCAL_REGION(handle->tracer, "recv,#F5E900FF", "clh.op")
        {
            CLH_Op op = {.request = request, .status_ptr = NULL};
            // CLH_PERF_REGION(handle, cache, register)
            TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")
            {
                bce = clh_buffer_cache_register_or_get(handle->buffer_cache,
                                                       op.request->data.recv.buffer);
                if (bce.mem != op.request->data.recv.buffer.mem) {
                    clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,
                             op.request->data.recv.buffer.mem);
                    clh_error("UCX", "%s", "registration error (recv).");
                    clh_request_release(handle, request);
                    return NULL;
                }
            }
            op.status_ptr = ucx_recv(handle, op.request, bce.memh);
            if (!validate_status_ptr_(handle, &op)) {
                clh_error("UCX", "%s", "ucx_recv request failure (recv).");
                clh_request_release(handle, request);
                return NULL;
            }
            WORKER_SIGNAL(handle);
        }
    }
    return request;
}

CLH_Request *clh_request_recv(CLH_Handle handle, CLH_Request *request, CLH_Buffer buffer)
{
    CLH_BufferCacheEntry bce;
    clh_u64              tag = request->data.probe.sender_tag;
    bool                 remove = request->data.probe.remove;
    ucp_tag_message_h    msg = request->data.probe.msg;

    request->type = CLH_REQUEST_TYPE_RECV;
    request->completed = false;
    request->data.recv.buffer = buffer;
    request->data.recv.tag = tag;
    request->data.recv.tag_mask = 0xFFFFFFFFFFFFFFFF;
    request->data.recv.msg = remove ? msg : NULL;
    CLH_PERF_REGION(handle, comm, recv)
    {
        TRACER_LOCAL_REGION(handle->tracer, "request recv,#F5E900FF", "clh.op")
        {
            CLH_Op op = {.request = request, .status_ptr = NULL};
            // CLH_PERF_REGION(handle, cache, register)
            TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")
            {
                bce = clh_buffer_cache_register_or_get(handle->buffer_cache,
                                                       op.request->data.recv.buffer);
                if (bce.mem != op.request->data.recv.buffer.mem) {
                    clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,
                             op.request->data.recv.buffer.mem);
                    clh_error("UCX", "%s", "registration error (recv).");
                    clh_request_release(handle, request);
                    return NULL;
                }
            }
            op.status_ptr = ucx_recv(handle, op.request, bce.memh);
            if (!validate_status_ptr_(handle, &op)) {
                clh_error("UCX", "%s", "ucx_recv request failure (recv).");
                clh_request_release(handle, request);
                return NULL;
            }
        }
    }
    return request;
}
#endif // CONF_USE_RECV_REQUEST_QUEUE

// probe ///////////////////////////////////////////////////////////////////////

#ifdef CONF_USE_PROBE_REQUEST_QUEUE
CLH_Request *clh_probe(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, bool remove)
{
    CLH_Request *request = clh_request_get(handle);
    request->type = CLH_REQUEST_TYPE_PROBE;
    request->completed = false;
    request->data.probe.result = false;
    request->data.probe.remove = remove;
    request->data.probe.tag = tag;
    request->data.probe.tag_mask = tag_mask;
    request->data.probe.msg = NULL;
    CLH_PERF_REGION(handle, comm, probe)
    {
        TRACER_LOCAL_REGION(handle->tracer, "probe,#00C99AFF", "clh.op")
        {
            CLH_LOCK_REGION(handle->request_queues[CLH_REQUEST_TYPE_PROBE].mutex)
            {
                array_append(handle->request_queues[CLH_REQUEST_TYPE_PROBE].requests, request);
            }
            WORKER_SIGNAL(handle);
            // CLH_PERF_REGION(handle, comm, probe_wait)
            TRACER_LOCAL_REGION(handle->tracer, "wait,#00C6C9FF", "clh.probe")
            {
                clh_wait(handle, request);
            }
        }
    }
    return request;
}
#else // CONF_USE_PROBE_REQUEST_QUEUE
CLH_Request *clh_probe(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, bool remove)
{
    CLH_Request *request = clh_request_get(handle);
    request->type = CLH_REQUEST_TYPE_PROBE;
    request->completed = false;
    request->data.probe.result = false;
    request->data.probe.remove = remove;
    request->data.probe.tag = tag;
    request->data.probe.tag_mask = tag_mask;
    request->data.probe.msg = NULL;
    CLH_PERF_REGION(handle, comm, probe)
    {
        TRACER_LOCAL_REGION(handle->tracer, "probe,#00C99AFF", "clh.op")
        {
            ucp_tag_recv_info_t infos;
            ucp_tag_message_h   msg;

            msg = ucp_tag_probe_nb(handle->worker, tag, tag_mask, remove, &infos);
            request->data.probe.result = (msg != NULL);
            request->data.probe.buffer_len = infos.length;
            request->data.probe.sender_tag = infos.sender_tag;
            request->data.probe.msg = msg;
        }
    }
    return request;
}
#endif // CONF_USE_PROBE_REQUEST_QUEUE

/******************************************************************************/
/*                                  requests                                  */
/******************************************************************************/

CLH_Status clh_wait(CLH_Handle, CLH_Request *request)
{
    CLH_LOCK_REGION(request->mutex)
    {
        while (!request->completed) {
            clh_conditional_variable_wait(&request->cv, &request->mutex);
        }
    }
    return CLH_STATUS_SUCCESS;
}

void clh_cancel(CLH_Handle, CLH_Request *)
{
    // TODO
    // if (request->status == NULL) {
    //     return;
    // }
    // clh_mutex_lock(&handle->mutex);
    // ucp_request_cancel(handle->worker, request->status);
    // clh_mutex_unlock(&handle->mutex);
}

bool clh_request_completed(CLH_Handle, CLH_Request *request)
{
    return request->completed;
}

struct CLH_RequestPoolNode *clh_request_pool_node_create()
{
    struct CLH_RequestPoolNode *node = malloc(sizeof(*node));
    assert((CLH_Request *)node == &node->request);
    node->request.mutex = clh_mutex_create();
    node->request.cv = clh_conditional_variable_create();
    return node;
}

void clh_request_pool_node_destroy(struct CLH_RequestPoolNode *node)
{
    clh_mutex_destroy(&node->request.mutex);
    clh_conditional_variable_destroy(&node->request.cv);
    free(node);
}

CLH_Request *clh_request_get(CLH_Handle handle)
{
    struct CLH_RequestPoolNode *node = NULL;

    CLH_LOCK_REGION(handle->request_pool.mutex)
    {
        if (handle->request_pool.free_nodes != NULL) {
            node = handle->request_pool.free_nodes;
            handle->request_pool.free_nodes = node->next;
        } else {
            node = clh_request_pool_node_create();
        }
    }
    return (CLH_Request *)node;
}

void clh_request_release(CLH_Handle handle, CLH_Request *request)
{
    CLH_LOCK_REGION(handle->request_pool.mutex)
    {
        struct CLH_RequestPoolNode *node = (struct CLH_RequestPoolNode *)request;
        node->next = handle->request_pool.free_nodes;
        handle->request_pool.free_nodes = node;
    }
}

size_t clh_request_buffer_len(CLH_Request *request)
{
    return request->data.probe.buffer_len;
}

clh_u64 clh_request_tag(CLH_Request *request)
{
    return request->data.probe.sender_tag;
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
