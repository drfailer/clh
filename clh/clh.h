#ifndef CLH
#define CLH
#include "array.h"
#include "buffer.h"
#include "cache.h"
#include "clh_defs.h"
#include "pmi.h"
#include "thread.h"
#include <ucp/api/ucp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CLH_HandleData *CLH_Handle;

typedef struct {
    const void *ptr;
    size_t len;
} CLH_AMHeader;
typedef bool (*CLH_AMHandler)(void *arg, CLH_AMHeader, CLH_Buffer buf);

typedef struct {
    CLH_Handle    handle;
    CLH_AMHandler user_callback;
    void         *user_callback_args;
} CLH_AMHandlerData;

Array(CLH_AMHandlerData) CLH_AMHandlerDataArray;

typedef enum {
    CLH_REQUEST_TYPE_SEND,
    CLH_REQUEST_TYPE_RECV,
    CLH_REQUEST_TYPE_PROBE,
    CLH_REQUEST_TYPE_AM_SEND,
    CLH_REQUEST_TYPE_SET_AM_HANDLER,
} CLH_RequestType;
#define CLH_NUMBER_REQUEST_TYPES 5

// TODO: one mutex + conditional variable per request + use union for clarity
typedef struct {
    CLH_RequestType         type;
    bool                    completed;
    CLH_Mutex               mutex;
    CLH_ConditionalVariable cv;
    union {
        struct {
            bool              result;
            bool              remove;
            clh_u64           tag;
            clh_u64           tag_mask;
            size_t            buffer_len;
            clh_u64           sender_tag;
            ucp_tag_message_h msg;
        } probe;
        struct {
            CLH_Buffer buffer;
            clh_u64    tag;
            clh_u32    dest;
        } send;
        struct {
            CLH_Buffer        buffer;
            clh_u64           tag;
            clh_u64           tag_mask;
            ucp_tag_message_h msg;
        } recv;
        struct {
            CLH_Buffer buffer;
            CLH_AMHeader header;
            clh_u32    dest;
            size_t handler_id;
        } am_send;
        struct {
            CLH_AMHandler handler;
            void *handler_args;
            size_t id;
        } set_am_handler;
    } data;
} CLH_Request;

Array(CLH_Request *) CLH_RequestArray;
typedef struct {
    CLH_Mutex        mutex;
    CLH_RequestArray requests;
} CLH_RequestQueue;

struct CLH_RequestPoolNode {
    CLH_Request                 request;
    struct CLH_RequestPoolNode *next;
};

typedef struct {
    CLH_Mutex                   mutex;
    struct CLH_RequestPoolNode *used_nodes; // TODO
    struct CLH_RequestPoolNode *free_nodes;
} CLH_RequestPool;

typedef struct {
    ucp_address_t *data;
    size_t         len;
} CLH_Address;

typedef struct {
    ucs_status_ptr_t *status_ptr;
    CLH_Request      *request;
} CLH_Op;

Array(CLH_Op) CLH_Ops;

struct CLH_HandleData {
    CLH_PMI_Handle   pmi;
    ucp_context_h    ucp_context;
    ucp_worker_h     worker;
    CLH_Address      address;
    ucp_ep_h        *endpoints;
    CLH_BufferCache *buffer_cache;
    bool             run;
    CLH_Thread       run_thread;
    CLH_Mutex        mutex;
    CLH_RequestQueue request_queues[CLH_NUMBER_REQUEST_TYPES];
    CLH_Ops          process_queue;
    CLH_RequestPool  request_pool;
    CLH_AMHandlerDataArray am_handlers_data;
};

typedef enum {
    CLH_STATUS_SUCCESS,
    CLH_STATUS_PMI_ERROR,
    CLH_STATUS_MEMORY_REGISTRATION_ERROR,
    CLH_STATUS_REQUEST_FAILURE,
    CLH_STATUS_ERROR,
} CLH_Status;

char const *clh_status_string(CLH_Status status);

CLH_Status clh_init(CLH_Handle *handle);
CLH_Status clh_finalize(CLH_Handle handle);

CLH_Request *clh_send(CLH_Handle handle, clh_u32 node_id, clh_u64 tag, CLH_Buffer buf);
CLH_Request *clh_recv(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Buffer buf);
CLH_Request *clh_request_recv(CLH_Handle handle, CLH_Request *probe_request, CLH_Buffer buf);
CLH_Request *clh_probe(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, bool remove);

CLH_Status clh_wait(CLH_Handle handle, CLH_Request *request);
void       clh_cancel(CLH_Handle handle, CLH_Request *request);

struct CLH_RequestPoolNode *clh_request_pool_node_create();
void                        clh_request_pool_node_destroy(struct CLH_RequestPoolNode *request);

CLH_Request *clh_request_get(CLH_Handle handle);
void         clh_request_release(CLH_Handle handle, CLH_Request *request);

bool    clh_request_completed(CLH_Handle handle, CLH_Request *request);
size_t  clh_request_buffer_len(CLH_Request *request);
clh_u64 clh_request_tag(CLH_Request *request);

void    clh_barrier(CLH_Handle handle);
clh_i32 clh_node_id(CLH_Handle handle);
clh_u32 clh_nb_nodes(CLH_Handle handle);

#ifdef __cplusplus
}
#endif

#endif // CLH
