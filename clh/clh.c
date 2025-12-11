#include "clh.h"
#include "log.h"
#include "pmi.h"
#include "protocol.h"
#include "ucx.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void *clh_request_allocate_(void *, size_t);
static void  clh_request_free_(void *, void *ptr);

// FIXME: since we are probing message manually, we should not put the worker
//        in pause as the run loop should keep looking for incomming messages.
//        We should still find a way to optimize the loop.
// #define CONF_WORKER_WAIT
#define CONF_WARMUP_LOOP_COUNT 10
#define CONF_PROGRESS_COUNT 1
#define CONF_PROBE_THRESH 1
#define CONF_SEND_THRESH 1024

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

#ifdef CONF_PROGRESS_COUNT
// TODO: we need an op counter in the handle
#define WORKER_PROGRESS(handle, count)                         \
    for (size_t i = 0; i < CONF_PROGRESS_COUNT; ++i) {         \
        size_t progress = ucp_worker_progress(handle->worker); \
        if (progress == 0) {                                   \
            break;                                             \
        }                                                      \
        count += progress;                                     \
    }
#else
#define WORKER_PROGRESS(handle, count)                         \
    while (true) {                                             \
        size_t progress = ucp_worker_progress(handle->worker); \
        if (progress == 0) {                                   \
            break;                                             \
        }                                                      \
        count += progress;                                     \
    }
#endif

/******************************************************************************/
/*                              init / finalize                               */
/******************************************************************************/

static void       start_(CLH_Handle handle);
static void       terminate_(CLH_Handle handle);
static CLH_Status clh_warmup_(CLH_Handle handle);

CLH_Status clh_init(CLH_Handle *handle)
{
    *handle = calloc(1, sizeof(struct CLH_HandleData));
    if (clh_pmi_init(&(*handle)->pmi) != CLH_PMI_STATUS_SUCCESS) {
        return CLH_STATUS_PMI_ERROR;
    }

    // we need the rank to determine the name of the tracer output file
    char trace_file[32] = {0};
    snprintf(trace_file, sizeof(trace_file), "clh_%d.tr", clh_node_id(*handle));
    (*handle)->tracer = TRACER_CREATE(trace_file, 0);

    (*handle)->mutex = clh_mutex_create();
    for (size_t i = 0; i < CLH_MAX_CHANNELS; ++i) {
        clh_list_init(&(*handle)->recv_list[i], .data_size = sizeof(CLH_Message),
                      .default_capacity = 10);
    }
    start_(*handle);
#ifdef CONF_WARMUP_LOOP_COUNT
    TRACER_DISABLE((*handle)->tracer);
    clh_warmup_(*handle);
    clh_barrier(*handle);
    TRACER_ENABLE((*handle)->tracer);
#endif
    return CLH_STATUS_SUCCESS;
}

CLH_Status clh_finalize(CLH_Handle handle)
{
    clh_pmi_sync(handle->pmi);
    terminate_(handle);
    clh_pmi_finalize(handle->pmi);
    clh_mutex_destroy(&handle->mutex);
    for (size_t i = 0; i < CLH_MAX_CHANNELS; ++i) {
        clh_list_destroy(&handle->recv_list[i]);
    }
    TRACER_DESTROY(handle->tracer);
    free(handle);
    return CLH_STATUS_SUCCESS;
}

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
/*                            ucx helper functions                            */
/******************************************************************************/

#define CLH_COMPLETE_REQUEST(request, ...)                 \
    do {                                                   \
        clh_mutex_lock(&request->mutex);                   \
        {__VA_ARGS__} request->completed = true;           \
        clh_conditional_variable_notify_one(&request->cv); \
        clh_mutex_unlock(&request->mutex);                 \
    } while (false);

static bool validate_status_ptr_(CLH_Handle handle, CLH_Op *op)
{
    if (UCS_PTR_IS_ERR(op->status_ptr)) {
        return false;
    }
    if (!UCS_PTR_IS_PTR(op->status_ptr)) {
        CLH_COMPLETE_REQUEST(op->request);
    } else {
        array_append(handle->ops_queue, *op);
    }
    return true;
}

/******************************************************************************/
/*                                    run                                     */
/******************************************************************************/

static CLH_ListNode *message_search(CLH_List *list, clh_u64 tag, clh_u64 tag_mask)
{
    CLH_ListNode *result = NULL;

    CLH_LOCK_REGION(list->mutex)
    {
        clh_list_foreach(CLH_Message, msg, list)
        {
            if ((msg->full_tag & tag_mask) == (tag & tag_mask)) {
                result = msg_cur;
                break;
            }
        }
    }
    return result;
}

#ifdef CONF_SEND_THRESH
static CLH_Status process_send_queue_(CLH_Handle handle)
{
    CLH_Status           status = CLH_STATUS_SUCCESS;
    CLH_BufferCacheEntry bce;
    CLH_ListNode        *node = NULL;

    if (handle->request_queues[CLH_REQUEST_TYPE_SEND].head == NULL) {
        return status;
    }

    while (handle->send_count < CONF_SEND_THRESH) {
        node = clh_list_pop_node(&handle->request_queues[CLH_REQUEST_TYPE_SEND]);
        if (node == NULL) {
            break;
        }

        CLH_Op op = {.request = (CLH_Request *)node->data, .status_ptr = NULL};
        TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")
        {
            bce = clh_buffer_cache_register_or_get(handle->buffer_cache,
                                                   op.request->data.send.buffer);
            if (bce.mem != op.request->data.send.buffer.mem) {
                clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,
                         op.request->data.send.buffer.mem);
                clh_error("UCX", "%s", "registration error.");
                return CLH_STATUS_MEMORY_REGISTRATION_ERROR;
            }
        }
        TRACER_LOCAL_REGION(handle->tracer, "ucx_send", "clh.ucx")
        {
            op.status_ptr = ucx_send(handle, op.request, bce.memh);
        }
        if (!validate_status_ptr_(handle, &op)) {
            clh_error("UCX", "%s", "send request failure.");
            return CLH_STATUS_REQUEST_FAILURE;
        }
        handle->send_count += 1;
    }
    return status;
}
#else
static CLH_Status process_send_queue_(CLH_Handle handle)
{
    CLH_Status           status = CLH_STATUS_SUCCESS;
    CLH_BufferCacheEntry bce;
    CLH_ListNode        *begin = NULL, *end = NULL, *cur = NULL;

    if (handle->request_queues[CLH_REQUEST_TYPE_SEND].head == NULL) {
        return status;
    }

    clh_list_get_all_nodes(&handle->request_queues[CLH_REQUEST_TYPE_SEND], &begin, &end);
    for (cur = begin; cur != NULL; cur = cur->next) {
        CLH_Op op = {.request = (CLH_Request *)cur->data, .status_ptr = NULL};
        TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")
        {
            bce = clh_buffer_cache_register_or_get(handle->buffer_cache,
                                                   op.request->data.send.buffer);
            if (bce.mem != op.request->data.send.buffer.mem) {
                clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,
                         op.request->data.send.buffer.mem);
                clh_error("UCX", "%s", "registration error.");
                return CLH_STATUS_MEMORY_REGISTRATION_ERROR;
            }
        }
        TRACER_LOCAL_REGION(handle->tracer, "ucx_send", "clh.ucx")
        {
            op.status_ptr = ucx_send(handle, op.request, bce.memh);
        }
        if (!validate_status_ptr_(handle, &op)) {
            clh_error("UCX", "%s", "send request failure.");
            return CLH_STATUS_REQUEST_FAILURE;
        }
    }
    return status;
}
#endif

static CLH_Status process_recv_queue_(CLH_Handle handle)
{
    CLH_Status           status = CLH_STATUS_SUCCESS;
    CLH_BufferCacheEntry bce;
    CLH_ListNode        *begin = NULL, *end = NULL, *cur = NULL;

    if (handle->request_queues[CLH_REQUEST_TYPE_RECV].head == NULL) {
        return status;
    }

    clh_list_get_all_nodes(&handle->request_queues[CLH_REQUEST_TYPE_RECV], &begin, &end);
    for (cur = begin; cur != NULL; cur = cur->next) {
        CLH_Op op = {.request = (CLH_Request *)cur->data, .status_ptr = NULL};
        TRACER_LOCAL_REGION(handle->tracer, "register", "clh.cache")
        {
            bce = clh_buffer_cache_register_or_get(handle->buffer_cache,
                                                   op.request->data.recv.buffer);
            if (bce.mem != op.request->data.recv.buffer.mem) {
                clh_info("UCX", "bce.mem = %p, buffer.mem = %p", bce.mem,
                         op.request->data.recv.buffer.mem);
                clh_error("UCX", "%s", "registration error.");
                return CLH_STATUS_MEMORY_REGISTRATION_ERROR;
            }
        }
        if (op.request->data.recv.msg == NULL) {
            CLH_Request  *request = (CLH_Request *)cur->data;
            CLH_ListNode *node
                = message_search(&handle->recv_list[op.request->channel], request->data.recv.tag,
                                 request->data.recv.tag_mask);

            if (node != NULL) {
                CLH_Message *msg = (CLH_Message *)node->data;
                op.request->data.recv.msg = msg->msg;
                op.request->channel = msg->channel;
                op.request->source = msg->sender_id;
                op.request->tag = msg->sender_tag;
                clh_list_remove_node(&handle->recv_list[request->channel], node);
            }
        }
        TRACER_LOCAL_REGION(handle->tracer, "ucx_recv", "clh.ucx")
        {
            op.status_ptr = ucx_recv(handle, op.request, bce.memh);
        }
        if (!validate_status_ptr_(handle, &op)) {
            clh_error("UCX", "%s", "recv request failure.");
            return CLH_STATUS_REQUEST_FAILURE;
        }
    }
    return status;
}

static inline CLH_Status process_shared_queues_(CLH_Handle handle)
{
    TRACER_LOCAL_REGION_CND(handle->request_queues[CLH_REQUEST_TYPE_SEND].head != NULL,
                            handle->tracer, "process send queue,#00990CFF", "clh.queues")
    {
        assert(process_send_queue_(handle) == CLH_STATUS_SUCCESS);
    }
    TRACER_LOCAL_REGION_CND(handle->request_queues[CLH_REQUEST_TYPE_RECV].head != NULL,
                            handle->tracer, "process recv queue,#F5E900FF", "clh.queues")
    {
        assert(process_recv_queue_(handle) == CLH_STATUS_SUCCESS);
    }
    return CLH_STATUS_SUCCESS;
}

static CLH_Status process_ops_queue_(CLH_Handle handle)
{
    CLH_Ops *queue = &handle->ops_queue;
    size_t   idx = 0;

    while (idx < queue->len) {
        CLH_Op      *op = &queue->ptr[idx];
        ucs_status_t status;

        assert(UCS_PTR_IS_PTR(op->status_ptr) && !UCS_PTR_IS_ERR(op->status_ptr));
        if (op->request->type == CLH_REQUEST_TYPE_RECV) {
            status = ucp_tag_recv_request_test(op->status_ptr, &op->request->data.recv.infos);
        } else {
            status = ucp_request_check_status(op->status_ptr);
        }

        if (status == UCS_INPROGRESS) {
            idx += 1;
        } else if (status == UCS_OK) {
            CLH_Request *request = op->request;

            if (request->type == CLH_REQUEST_TYPE_RECV) {
                clh_decode_tag(request->data.recv.infos.sender_tag, &request->channel,
                               &request->source, &request->tag);
            }
            ucp_request_free(op->status_ptr);
            array_remove(handle->ops_queue, idx);
#ifdef CONF_SEND_THRESH
            if (request->type == CLH_REQUEST_TYPE_SEND) {
                handle->send_count -= 1;
            }
#endif
            CLH_COMPLETE_REQUEST(request);
        } else {
            clh_error("UCX", "request error (status = %s)", ucs_status_string(status));
            array_remove(handle->ops_queue, idx);
            return CLH_STATUS_ERROR;
        }
    }
    return CLH_STATUS_SUCCESS;
}

static bool queues_emtpy_(CLH_Handle handle)
{
    return handle->ops_queue.len == 0 && handle->request_queues[CLH_REQUEST_TYPE_SEND].len == 0
           && handle->request_queues[CLH_REQUEST_TYPE_RECV].len == 0
           && handle->request_queues[CLH_REQUEST_TYPE_PROBE].len == 0;
}

static void probe_incomming_messages_(CLH_Handle handle)
{
    CLH_Message         msg;
    ucp_tag_recv_info_t infos;

    for (size_t i = 0; i < CONF_PROBE_THRESH; ++i) {
        msg.msg = ucp_tag_probe_nb(handle->worker, 0, 0, 1, &infos);
        if (msg.msg == NULL) {
            break;
        }
        msg.full_tag = infos.sender_tag;
        clh_decode_tag(infos.sender_tag, &msg.channel, &msg.sender_id, &msg.sender_tag);
        msg.buffer_len = infos.length;
        clh_list_push_data(&handle->recv_list[msg.channel], &msg);
    }
}

static void *run_(void *arg)
{
    CLH_Handle handle = (CLH_Handle)arg;

    assert(ucx_init(handle) == CLH_STATUS_SUCCESS);

    handle->run = true; // make sure ucx is inited
    clh_conditional_variable_notify_one(&handle->init_cv);

    while (handle->run || !queues_emtpy_(handle)) {
        probe_incomming_messages_(handle);
        WORKER_WAIT(handle);
#ifdef ENABLE_TRACER
        size_t nb_send = handle->request_queues[CLH_REQUEST_TYPE_SEND].len;
        size_t nb_recv = handle->request_queues[CLH_REQUEST_TYPE_RECV].len;
        size_t nb_probe = handle->request_queues[CLH_REQUEST_TYPE_PROBE].len;
#endif
        TRACER_LOCAL_REGION_CND(!queues_emtpy_(handle), handle->tracer,
                                "process shared queues,#F7C48BFF", "clh.worker",
                                "node = %d\nsend queue size = %ld\nrecv queue size = "
                                "%ld\nprobe queue size = %ld",
                                clh_node_id(handle), nb_send, nb_recv, nb_probe)
        {
            process_shared_queues_(handle);
        }
        size_t progress_count = 0;
        TRACER_LOCAL_REGION_CND(progress_count > 0, handle->tracer, "progress worker,#CC0000FF",
                                "clh.worker", "node = %d\nprogress count = %ld",
                                clh_node_id(handle), progress_count)
        {
            WORKER_PROGRESS(handle, progress_count);
        }
        TRACER_LOCAL_REGION_CND(
            handle->ops_queue.len > 0, handle->tracer, "process ops queue,#009BC2FF", "clh.worker",
            "node = %d\nqueue size = %ld", clh_node_id(handle), handle->ops_queue.len)
        {
            assert(process_ops_queue_(handle) == CLH_STATUS_SUCCESS);
        }
    }
    assert(ucx_finalize(handle) == CLH_STATUS_SUCCESS);
    printf("CLH RUN TERMINATE\n");
    return 0;
}

#define request_queue_init(queue, c, a) \
    clh_list_init(&queue, .data_size = sizeof(CLH_Request), .default_capacity = c, .allocator = a)

static void start_(CLH_Handle handle)
{
    CLH_Allocator request_allocator = {
        .allocate = clh_request_allocate_,
        .free = clh_request_free_,
    };
    request_queue_init(handle->request_queues[CLH_REQUEST_TYPE_SEND], 10, request_allocator);
    request_queue_init(handle->request_queues[CLH_REQUEST_TYPE_RECV], 10, request_allocator);
    request_queue_init(handle->request_queues[CLH_REQUEST_TYPE_PROBE], 10, request_allocator);
    array_create(handle->ops_queue, 1024);

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
    clh_list_destroy(&handle->request_queues[CLH_REQUEST_TYPE_SEND]);
    clh_list_destroy(&handle->request_queues[CLH_REQUEST_TYPE_RECV]);
    clh_list_destroy(&handle->request_queues[CLH_REQUEST_TYPE_PROBE]);
    array_destroy(handle->ops_queue);
}

/******************************************************************************/
/*                                   warmup                                   */
/******************************************************************************/

#ifdef CONF_WARMUP_LOOP_COUNT
#define WARMUP_MSG_SIZE 1024 * 1024
typedef struct {
    char str[WARMUP_MSG_SIZE];
} WarmupMem;
Array(WarmupMem) WarmupMemArray;

static CLH_Status clh_warmup_(CLH_Handle handle)
{
    CLH_RequestArray send_requests = {0}, recv_requests = {0};
    WarmupMemArray   recv_mem = {0};
    char             send_mem[WARMUP_MSG_SIZE] = {0}, expected_response[WARMUP_MSG_SIZE] = {0};
    clh_u64          node_id = clh_node_id(handle);
    clh_u64          nb_nodes = clh_nb_nodes(handle);

    array_create(send_requests, CONF_WARMUP_LOOP_COUNT * nb_nodes);
    array_create(recv_requests, CONF_WARMUP_LOOP_COUNT * nb_nodes);
    array_create(recv_mem, CONF_WARMUP_LOOP_COUNT * nb_nodes);
    snprintf(send_mem, WARMUP_MSG_SIZE, "warmup send from %ld", node_id);

    for (size_t i = 0; i < CONF_WARMUP_LOOP_COUNT; ++i) {
        for (size_t rank = 0; rank < nb_nodes; ++rank) {
            if (rank == node_id) {
                continue;
            }
            size_t     idx = i * nb_nodes + rank;
            CLH_Buffer send_buf = {send_mem, WARMUP_MSG_SIZE},
                       recv_buf = {recv_mem.ptr[idx].str, WARMUP_MSG_SIZE};
            send_requests.ptr[idx] = clh_send(handle, 0, rank, 0, send_buf);
            recv_requests.ptr[idx] = clh_recv_from(handle, 0, rank, 0, 0xFFFFFFFF, recv_buf);
        }
    }

    for (size_t i = 0; i < CONF_WARMUP_LOOP_COUNT; ++i) {
        for (size_t rank = 0; rank < nb_nodes; ++rank) {
            if (rank == node_id) {
                continue;
            }
            size_t idx = i * nb_nodes + rank;
            clh_wait(handle, send_requests.ptr[idx]);
            clh_request_release(handle, send_requests.ptr[idx]);
        }
    }

    for (size_t i = 0; i < CONF_WARMUP_LOOP_COUNT; ++i) {
        for (size_t rank = 0; rank < nb_nodes; ++rank) {
            if (rank == node_id) {
                continue;
            }
            size_t idx = i * nb_nodes + rank;
            snprintf(expected_response, WARMUP_MSG_SIZE, "warmup send from %ld", rank);
            clh_wait(handle, recv_requests.ptr[idx]);
            clh_request_release(handle, recv_requests.ptr[idx]);
            if (strcmp(expected_response, recv_mem.ptr[idx].str) != 0) {
                printf("received: %s, expect: %s, cmp = %d\n", recv_mem.ptr[idx].str,
                       expected_response, strcmp(expected_response, recv_mem.ptr[idx].str));
                assert(false && "warmup test failed.");
            }
        }
    }

    array_destroy(send_requests);
    array_destroy(recv_requests);
    array_destroy(recv_mem);
    return CLH_STATUS_SUCCESS;
}
#endif

/******************************************************************************/
/*                          communication functions                           */
/******************************************************************************/

// send ////////////////////////////////////////////////////////////////////////

CLH_Request *clh_send(CLH_Handle handle, clh_u32 channel, clh_u32 dest, clh_u32 tag,
                      CLH_Buffer buffer)
{
    CLH_ListNode *node = clh_list_new_node(&handle->request_queues[CLH_REQUEST_TYPE_SEND]);
    CLH_Request  *request = (CLH_Request *)node->data;
    assert(request != NULL);
    request->type = CLH_REQUEST_TYPE_SEND;
    request->completed = false;
    request->channel = channel;
    request->tag = tag;
    request->source = clh_node_id(handle);
    request->data.send.buffer = buffer;
    request->data.send.tag = clh_encode_tag(channel, request->source, tag);
    request->data.send.dest = dest;
    TRACER_LOCAL_REGION(handle->tracer, "send,#00990CFF", "clh.op",
                        "node = %d\ndest = %d\ntag = %ld\nbuffer = { %p; %ld }",
                        clh_node_id(handle), dest, tag, buffer.mem, buffer.len)
    {
        clh_list_push_node(&handle->request_queues[CLH_REQUEST_TYPE_SEND], node);
        WORKER_SIGNAL(handle);
    }
    return request;
}

// recv ////////////////////////////////////////////////////////////////////////

static CLH_Request *clh_recv_(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Buffer buffer)
{
    CLH_ListNode *node = clh_list_new_node(&handle->request_queues[CLH_REQUEST_TYPE_RECV]);
    CLH_Request  *request = (CLH_Request *)node->data;
    assert(request != NULL);
    request->type = CLH_REQUEST_TYPE_RECV;
    request->completed = false;
    request->data.recv.buffer = buffer;
    request->data.recv.tag = tag;
    request->data.recv.tag_mask = tag_mask;
    request->data.recv.msg = NULL;
    TRACER_LOCAL_REGION(handle->tracer, "recv,#F5E900FF", "clh.op",
                        "node = %d\ntag = %ld\ntag_mask = %ld\nbuffer = { %p; %ld }",
                        clh_node_id(handle), tag, tag_mask, buffer.mem, buffer.len)
    {
        clh_list_push_node(&handle->request_queues[CLH_REQUEST_TYPE_RECV], node);
        WORKER_SIGNAL(handle);
    }
    return request;
}

CLH_Request *clh_recv(CLH_Handle handle, clh_u32 channel, clh_u32 tag, clh_u32 tag_mask,
                      CLH_Buffer buffer)
{
    clh_u64 clh_tag = clh_encode_tag(channel, 0, tag);
    clh_u64 clh_tag_mask = CHANNEL_MASK | (clh_u64)tag_mask;
    return clh_recv_(handle, clh_tag, clh_tag_mask, buffer);
}

CLH_Request *clh_recv_from(CLH_Handle handle, clh_u32 channel, clh_u32 source, clh_u32 tag,
                           clh_u32 tag_mask, CLH_Buffer buffer)
{
    clh_u64 clh_tag = clh_encode_tag(channel, source, tag);
    clh_u64 clh_tag_mask = CHANNEL_MASK | SENDER_ID_MASK | (clh_u64)tag_mask;
    return clh_recv_(handle, clh_tag, clh_tag_mask, buffer);
}

CLH_Request *clh_request_recv(CLH_Handle handle, CLH_Request *request, CLH_Buffer buffer)
{
    clh_u64           tag = request->data.probe.sender_tag;
    bool              remove = request->data.probe.remove;
    ucp_tag_message_h msg = request->data.probe.msg;

    // we don't change the type so that the request returns to the proper pool
    request->completed = false;
    request->data.recv.buffer = buffer;
    request->data.recv.tag = tag;
    request->data.recv.tag_mask = 0xFFFFFFFFFFFFFFFF;
    request->data.recv.msg = remove ? msg : NULL;
    TRACER_LOCAL_REGION(handle->tracer, "request recv,#F5E900FF", "clh.op",
                        "node = %d\ntag = %ld\nremove = %d\nmsg = %p", clh_node_id(handle), tag,
                        remove, msg)
    {
        clh_list_push_node(&handle->request_queues[CLH_REQUEST_TYPE_RECV],
                           clh_list_node_from_data(request));
        WORKER_SIGNAL(handle);
    }
    return request;
}

// probe ///////////////////////////////////////////////////////////////////////

CLH_Request *clh_probe_(CLH_Handle handle, clh_u32 channel, clh_u64 tag, clh_u64 tag_mask,
                        bool remove)
{
    CLH_ListNode *node = clh_list_new_node(&handle->request_queues[CLH_REQUEST_TYPE_PROBE]);
    CLH_Request  *request = (CLH_Request *)node->data;
    request->type = CLH_REQUEST_TYPE_PROBE;
    request->completed = true;
    request->data.probe.result = false;
    request->data.probe.remove = remove;
    request->data.probe.tag = tag;
    request->data.probe.tag_mask = tag_mask;
    request->data.probe.msg = NULL;
    TRACER_LOCAL_REGION(handle->tracer, "probe,#00C99AFF", "clh.op",
                        "node = %d\ntag = %ld\ntag_mask = %ld\nremove = %d", clh_node_id(handle),
                        tag, tag_mask, remove)
    {
        CLH_ListNode *node = message_search(&handle->recv_list[channel], tag, tag_mask);
        if (node != NULL) {
            CLH_Message *msg = (CLH_Message *)node->data;
            request->data.probe.result = true;
            request->data.probe.sender_tag = msg->full_tag;
            request->data.probe.buffer_len = msg->buffer_len;
            request->data.probe.msg = msg->msg;
            request->channel = msg->channel;
            request->source = msg->sender_id;
            request->tag = msg->sender_tag;
            if (remove) {
                clh_list_remove_node(&handle->recv_list[channel], node);
            }
        }
    }
    return request;
}

CLH_Request *clh_probe(CLH_Handle handle, clh_u32 channel, clh_u32 tag, clh_u32 tag_mask,
                       bool remove)
{
    clh_u64 clh_tag = clh_encode_tag(channel, 0, tag);
    clh_u64 clh_tag_mask = CHANNEL_MASK | (clh_u64)tag_mask;
    return clh_probe_(handle, channel, clh_tag, clh_tag_mask, remove);
}

CLH_Request *clh_probe_source(CLH_Handle handle, clh_u32 channel, clh_u32 source, clh_u32 tag,
                              clh_u32 tag_mask, bool remove)
{
    clh_u64 clh_tag = clh_encode_tag(channel, source, tag);
    clh_u64 clh_tag_mask = CHANNEL_MASK | SENDER_ID_MASK | (clh_u64)tag_mask;
    return clh_probe_(handle, channel, clh_tag, clh_tag_mask, remove);
}

/******************************************************************************/
/*                                  requests                                  */
/******************************************************************************/

CLH_Status clh_wait(CLH_Handle, CLH_Request *request)
{
    assert(request != NULL);
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

static void *clh_request_allocate_(void *, size_t size)
{
    CLH_ListNode *node = malloc(size);
    CLH_Request  *request = (CLH_Request *)node->data;
    request->mutex = clh_mutex_create();
    request->cv = clh_conditional_variable_create();
    return node;
}

static void clh_request_free_(void *, void *ptr)
{
    CLH_ListNode *node = (CLH_ListNode *)ptr;
    CLH_Request  *request = (CLH_Request *)node->data;
    clh_mutex_destroy(&request->mutex);
    clh_conditional_variable_destroy(&request->cv);
    free(node);
}

void clh_request_release(CLH_Handle handle, CLH_Request *request)
{
    clh_list_release_data(&handle->request_queues[request->type], request);
}

size_t clh_request_buffer_len(CLH_Request *request)
{
    return request->data.probe.buffer_len;
}

clh_u32 clh_request_channel(CLH_Request *request)
{
    return request->channel;
}

clh_i32 clh_request_source(CLH_Request *request)
{
    return request->source;
}

clh_u32 clh_request_tag(CLH_Request *request)
{
    return request->tag;
}

bool clh_probe_result(CLH_Request *request)
{
    return request->data.probe.result;
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
