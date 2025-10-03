#ifndef CLH
#define CLH
#include "buffer.h"
#include "cache.h"
#include "clh_defs.h"
#include "pmi.h"
#include "thread.h"
#include "array.h"
#include <ucp/api/ucp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLH_REQUEST_TYPE_SEND,
    CLH_REQUEST_TYPE_RECV,
    CLH_REQUEST_TYPE_PROBE,
} CLH_RequestType;
#define CLH_NUMBER_REQUEST_TYPES 3

typedef struct {
    CLH_RequestType     type;
    volatile bool       completed;
    CLH_Buffer          buffer;
    clh_u64             tag;
    clh_u64             tag_mask;
    clh_u32             dest;
    ucp_tag_recv_info_t tag_recv_info;
    bool                probe;
} CLH_Request;

typedef struct {
    ucp_address_t *data;
    size_t         len;
} CLH_Address;

typedef bool (*CLH_AMHandler)(void *arg, CLH_Buffer header, CLH_Buffer buf);

typedef struct {
    CLH_AMHandler user_callback;
    void         *user_callback_args;
} CLH_AMHandlerData;

typedef struct {
    ucs_status_ptr_t *status_ptr;
    CLH_Request      *request;
} CLH_Op;


Array(CLH_Op) CLH_Ops;

Array(CLH_Request*) CLH_RequestArray;
typedef struct {
    CLH_Mutex mutex;
    CLH_RequestArray requests;
} CLH_RequestQueue;

// TODO:
// - write a request pool that will contain two lists (request allocation):
//   - free list
//   - non free list (dynamically filled when the free list is empty)
// - each node will have a request (that can be return via the clh interface),
//   and a condition variable (for waiting if necessary).

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
    // contains the request queues for all the types
    CLH_RequestQueue request_queues[CLH_NUMBER_REQUEST_TYPES];
    CLH_Ops          send_queue;
    CLH_Ops          recv_queue;
    CLH_Ops          probe_queue;
    CLH_Ops          process_queue;
};

typedef struct CLH_HandleData *CLH_Handle;

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

CLH_Status clh_send(CLH_Handle handle, clh_u32 node_id, clh_u64 tag, CLH_Buffer buf,
                    CLH_Request *request);
CLH_Status clh_recv(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Buffer buf,
                    CLH_Request *request);

bool       clh_probe(CLH_Handle handle, clh_u64 tag, clh_u64 tag_mask, CLH_Request *request);
CLH_Status clh_wait(CLH_Handle handle, CLH_Request *request);
void       clh_cancel(CLH_Handle handle, CLH_Request *request);

CLH_Request *clh_request_create();
void        clh_request_destroy(CLH_Request *request);

bool        clh_request_completed(CLH_Handle handle, CLH_Request *request);
size_t      clh_request_buffer_len(CLH_Request *request);
clh_u64     clh_request_tag(CLH_Request *request);

void clh_barrier(CLH_Handle handle);
clh_i32 clh_node_id(CLH_Handle handle);
clh_u32 clh_nb_nodes(CLH_Handle handle);

#ifdef __cplusplus
}
#endif

#endif // CLH
