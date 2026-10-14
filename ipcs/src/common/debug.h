#ifndef __DEBUG_H__
#define __DEBUG_H__
#define PERF 0
#define LOGS 1
#define CONS 2
#define DEBUG LOGS
#if DEBUG >=LOGS
    #define gt_debug_logs(stream, format, ...) \
        fprintf((stream), (format), ##__VA_ARGS__)
#else 
    #define gt_debug_logs(stream, format, ...) 
#endif
#if DEBUG >=CONS
    #define gt_debug(stream, format, ...) \
        fprintf((stream), (format), ##__VA_ARGS__)
#else 
    #define gt_debug(stream, format, ...) 
#endif
#endif