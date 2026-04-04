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

#include <stdio.h>       /* printf, fflush */
#include <stdint.h>
#include "stm32h7xx_hal.h"  /* HAL_GetTick() */
#include "SEGGER_RTT.h"

#define LOG(fmt, ...) do {                          \
    printf(fmt, ##__VA_ARGS__);                     \
    fflush(stdout);                                 \
} while (0)

/* RTT_TS — write a timestamped line to RTT channel 0.
 * Prepends "[T+xxxxxxx] " (HAL_GetTick ms since boot).
 * Use this instead of SEGGER_RTT_WriteString for all app log lines. */
#define RTT_TS(msg) do {                                                    \
    char _ts[16];                                                           \
    int _n = snprintf(_ts, sizeof(_ts), "[T+%7lu] ",                       \
                      (unsigned long)HAL_GetTick());                        \
    if (_n > 0) SEGGER_RTT_Write(0, _ts, (unsigned)_n);                   \
    SEGGER_RTT_WriteString(0, (msg));                                       \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* LOG_MUTEX_H */
