#include <stdio.h>
#include <assert.h>
#include <clh/clh.h>
#include <string.h>

int main(int, char **)
{
    CLH_Handle clh = NULL;
    char message[1024] = {0};

    if (clh_init(&clh) != CLH_STATUS_SUCCESS) {
        return 1;
    }

    if (clh_node_id(clh) == 0) {
        for (size_t i = 1; i < clh_nb_nodes(clh); ++i) {
            CLH_Request *request = clh_recv(clh, 0, 0, 0, (CLH_Buffer){message, 1024});
            // CLH_Request *request = clh_recv_from(clh, 0, i, 0, 0, (CLH_Buffer){message, 1024});
            assert(request);
            TRACER_LOCAL_REGION(clh->tracer, "recv wait,#FF0000FF", "main")
            {
                clh_wait(clh, request);
            }
            printf("message received: `%s`\n", message);
            clh_request_release(clh, request);
        }
    } else {
        sprintf(message, "Hello from rank = %d", clh_node_id(clh));
        CLH_Request *request = clh_send(clh, 0, 0, 0, (CLH_Buffer){message, strlen(message) + 1});
        assert(request);
        TRACER_LOCAL_REGION(clh->tracer, "send wait,#00FF00FF", "main")
        {
            clh_wait(clh, request);
        }
        clh_request_release(clh, request);
    }

    clh_finalize(clh);
}
