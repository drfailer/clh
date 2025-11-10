#include "clh_perf.h"

CLH_TimePoint clh_perf_tp_get()
{
    CLH_TimePoint tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return tp;
}

CLH_Duration clh_perf_tp_dur(CLH_TimePoint t_start, CLH_TimePoint t_end)
{
    return (1000.0 * t_end.tv_sec + 1e-6 * t_end.tv_nsec) - (1000.0 * t_start.tv_sec + 1e-6 * t_start.tv_nsec);
}
