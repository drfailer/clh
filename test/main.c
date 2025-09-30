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
            CLH_Request request = clh_request_create();
            CLH_Status status = clh_recv(clh, 0, 0, (CLH_Buffer){message, 1024}, request);
            assert(status == CLH_STATUS_SUCCESS);
            clh_wait(clh, request);
            printf("message received: `%s`\n", message);
            clh_request_destroy(request);
        }
    } else {
        CLH_Request request = clh_request_create();
        sprintf(message, "Hello from rank = %d", clh_node_id(clh));
        CLH_Status status = clh_send(clh, 0, 0, (CLH_Buffer){message, strlen(message) + 1}, request);
        assert(status == CLH_STATUS_SUCCESS);
        clh_wait(clh, request);
        clh_request_destroy(request);
    }

    clh_finalize(clh);
}
