#ifndef CLH_PERF
#define CLH_PERF
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct timespec CLH_TimePoint;
typedef double CLH_Duration;

CLH_TimePoint clh_perf_tp_get();
CLH_Duration clh_perf_tp_dur(CLH_TimePoint t_start, CLH_TimePoint t_end);

#define clh_perf_timer_start(timer_name) CLH_TimePoint start_##timer_name = clh_perf_tp_get();
#define clh_perf_timer_end(timer_name) CLH_TimePoint end_##timer_name = clh_perf_tp_get();
#define clh_perf_timer_dur(timer_name) clh_perf_tp_dur(start_##timer_name, end_##timer_name);

#ifdef __cplusplus
}
#endif

#endif
