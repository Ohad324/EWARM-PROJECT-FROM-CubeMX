/**
 * @file    log_mutex.h
 * @brief   Logging wrapper — output via printf routed to SWO (IAR Terminal I/O).
 *
 * Switched from SEGGER RTT to printf. IAR routes printf to SWO when you set:
 *   Project -> Options -> General Options -> Library I/O -> SWO
 * Output appears in the Terminal I/O window during debug. Non-blocking, no CPU halt.
 *
 * NOTE: do NOT call LOG() from ISR context.
 *
 * Usage:
 *   LOG("[Task] value = %d\n", val);
 */

#ifndef LOG_MUTEX_H
#define LOG_MUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>   /* printf, fflush */

#define LOG(fmt, ...) do {                          \
    printf(fmt, ##__VA_ARGS__);                     \
    fflush(stdout);                                 \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* LOG_MUTEX_H */
