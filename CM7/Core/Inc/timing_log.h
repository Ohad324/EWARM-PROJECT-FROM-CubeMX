#pragma once
/**
 * @file  timing_log.h
 * @brief Timing log with DWT microsecond timestamps — output via printf routed to SWO.
 *
 * Switched from SEGGER RTT to printf. IAR routes printf to SWO when you set:
 *   Project -> Options -> General Options -> Library I/O -> SWO
 * Output appears in the Terminal I/O window during debug. Non-blocking, no CPU halt.
 *
 * NOTE: do NOT call TLOG() from ISR context.
 *
 * T_US()  — DWT->CYCCNT / 480 (microseconds at 480 MHz, wraps ~89 s)
 * TLOG()  — writes "[TIMING] <text>\n" to IAR Terminal I/O via SWO
 */

#include <stdio.h>   /* printf, fflush */

#ifdef __cplusplus
extern "C" {
#endif

/* DWT CYCCNT via raw address — works in C and C++ without CMSIS headers */
#define T_US()  (*(volatile uint32_t *)0xE0001004UL / 480UL)

#define TLOG(fmt, ...) do {                                          \
    printf("[TIMING] " fmt "\n", ##__VA_ARGS__);                    \
    fflush(stdout);                                                  \
} while (0)

#ifdef __cplusplus
}
#endif
