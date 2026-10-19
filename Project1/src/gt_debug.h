#ifndef __GT_DEBUG_H
#define __GT_DEBUG_H
#define PERF 0
#define LOGS 1
#define CONS 2
#define DEBUG PERF
#if DEBUG >=LOGS
    #define gt_debug_logs(stream, format, ...) \
        atomic_fprintf((stream), (format), ##__VA_ARGS__)
#else 
    #define gt_debug_logs(stream, format, ...) 
#endif
#if DEBUG >=CONS
    #define gt_debug(stream, format, ...) \
        atomic_fprintf((stream), (format), ##__VA_ARGS__)
#else 
    #define gt_debug(stream, format, ...) 
#endif
#endif