#include "ucx_utils.h"

bool check_ucx_(ucs_status_t status, char const *filename, size_t line)
{
    if (status == UCS_OK) {
        return true;
    }
    fprintf(stderr, "%s:%ld\tCLH UCX ERROR\t%s\n", filename, line, ucs_status_string(status));
    return false;
}
