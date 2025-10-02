#include "log.h"
#include <stdio.h>

char const *clh_log_level_string(CLH_LogLevel level)
{
    static char value[8];

    switch (level) {
    case CLH_LOG_INFO:
        snprintf(value, sizeof(value), "INFO   ");
        break;
    case CLH_LOG_WARNING:
        snprintf(value, sizeof(value), "WARNING");
        break;
    case CLH_LOG_ERROR:
        snprintf(value, sizeof(value), "ERROR  ");
        break;
    }
    return value;
}

void clh_log_(FILE *file, CLH_LogLevel level, char const *category, char const *filename,
              size_t line, char const *msg)
{
    fprintf(file, "CLH   %s  %s\t%s:%ld\t%s\n", clh_log_level_string(level), category, filename,
            line, msg);
}

bool check_ucx_(ucs_status_t status, char const *filename, size_t line)
{
    if (status == UCS_OK) {
        return true;
    }
    clh_log_(stderr, CLH_LOG_ERROR, "UCX", filename, line, ucs_status_string(status));
    return false;
}
