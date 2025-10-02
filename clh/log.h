#ifndef CLH_LOG
#define CLH_LOG
#include <stdbool.h>
#include <ucp/api/ucp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CLH_LOG_INFO,
    CLH_LOG_WARNING,
    CLH_LOG_ERROR,
} CLH_LogLevel;

char const *clh_log_level_string(CLH_LogLevel level);

#define clh_log(file, level, category, fmt, ...)                                           \
    fprintf(file, "CLH   %s  %s\t%s:%d\t" fmt "\n", clh_log_level_string(level), category, \
            __FILE__, __LINE__, __VA_ARGS__);
#define clh_info(category, fmt, ...) clh_log(stdout, CLH_LOG_INFO, category, fmt, __VA_ARGS__)
#define clh_warning(category, fmt, ...) clh_log(stderr, CLH_LOG_WARNING, category, fmt, __VA_ARGS__)
#define clh_error(category, fmt, ...) clh_log(stderr, CLH_LOG_ERROR, category, fmt, __VA_ARGS__)

bool check_ucx_(ucs_status_t status, char const *filename, size_t line);
#define check_ucx(status) check_ucx_(status, __FILE__, __LINE__)

#ifdef __cplusplus
}
#endif

#endif // CLH_LOG
