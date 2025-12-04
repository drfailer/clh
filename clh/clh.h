#ifndef CLH
#define CLH
#include "array.h"
#include "buffer.h"
#include "cache.h"
#include "clh_defs.h"
#include "list.h"
#include "mem.h"
#include "pmi.h"
#include "thread.h"
#include <ucp/api/ucp.h>
// #define ENABLE_TRACER
#include <tracer.h>

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

typedef struct CLH_HandleData *CLH_Handle;

typedef struct CLH_Request CLH_Request;

typedef enum {
    CLH_REQUEST_TYPE_SEND,
    CLH_REQUEST_TYPE_RECV,
    CLH_REQUEST_TYPE_PROBE,
} CLH_RequestType;
#define CLH_NUMBER_REQUEST_TYPES 6

struct CLH_Request {
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
    } data;
};

Array(CLH_Request *) CLH_RequestArray;

typedef struct {
    ucp_address_t *data;
    size_t         len;
} CLH_Address;

typedef struct {
    ucs_status_ptr_t *status_ptr;
    CLH_Request      *request;
} CLH_Op;

Array(CLH_Op) CLH_Ops;

typedef struct {
    clh_u32           channel;
    clh_i32           sender_id;
    clh_u32           sender_tag;
    clh_u64           buffer_len;
    ucp_tag_message_h msg;
} CLH_Message;

#define CLH_MAX_CHANNELS 256

struct CLH_HandleData {
    CLH_PMI_Handle          pmi;
    ucp_context_h           ucp_context;
    ucp_worker_h            worker;
    CLH_Address             address;
    ucp_ep_h               *endpoints;
    CLH_BufferCache        *buffer_cache;
    volatile bool           run;
    CLH_Thread              run_thread;
    CLH_Mutex               mutex;
    CLH_List                request_queues[CLH_NUMBER_REQUEST_TYPES];
    CLH_Ops                 ops_queue;
    TracerHandle           *tracer;
    CLH_ConditionalVariable init_cv;
    CLH_List                recv_list[CLH_MAX_CHANNELS];
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

CLH_Request *clh_send(CLH_Handle handle, clh_u32 channel, clh_u32 dest, clh_u32 tag,
                      CLH_Buffer buffer);
CLH_Request *clh_recv(CLH_Handle handle, clh_u32 channel, clh_u32 source, clh_u32 tag,
                      clh_u32 tag_mask, CLH_Buffer buffer);
CLH_Request *clh_request_recv(CLH_Handle handle, CLH_Request *probe_request, CLH_Buffer buf);
CLH_Request *clh_probe(CLH_Handle handle, clh_u32 channel, clh_u32 tag, clh_u32 tag_mask,
                       bool remove);
CLH_Request *clh_probe_source(CLH_Handle handle, clh_u32 channel, clh_u32 source, clh_u32 tag,
                              clh_u32 tag_mask, bool remove);

CLH_Status clh_wait(CLH_Handle handle, CLH_Request *request);
void       clh_cancel(CLH_Handle handle, CLH_Request *request);

CLH_Request *clh_request_get(CLH_Handle handle);
void         clh_request_release(CLH_Handle handle, CLH_Request *request);

bool    clh_request_completed(CLH_Handle handle, CLH_Request *request);
size_t  clh_request_buffer_len(CLH_Request *request);
clh_u64 clh_request_tag(CLH_Request *request);

void    clh_barrier(CLH_Handle handle);
clh_i32 clh_node_id(CLH_Handle handle);
clh_u32 clh_nb_nodes(CLH_Handle handle);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif // CLH
