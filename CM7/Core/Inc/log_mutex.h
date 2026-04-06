/**
 * @file    log_mutex.h
 * @brief   Logging wrapper — RTT (J-Link RTT Viewer) + SWO (IAR Terminal I/O).
 *
 * RTT and SWO are independent hardware paths — both active simultaneously:
 *   RTT : SEGGER memory buffer, read by J-Link over SWD. No extra pin needed.
 *   SWO : dedicated SWO pin, routed via ITM/TPIU. Viewed in IAR Terminal I/O.
 *         Requires: Project → Options → General Options → Library I/O → SWO
 *
 * RTT_TS(msg)      — timestamped fixed string → RTT + SWO
 * SD_LOG(fmt, ...) — timestamped printf-style → RTT + SWO  (defined in audio_sd.c)
 *
 * NOTE: do NOT call from ISR context.
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

/* TS — print timestamp prefix to SWO Terminal I/O. */
#define TS() printf("[T+%7lu] ", (unsigned long)HAL_GetTick())

/* LOG — printf-style to SWO Terminal I/O (no timestamp). */
#define LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)

/* LOG_TS — timestamped printf-style to SWO Terminal I/O. */
#define LOG_TS(fmt, ...) do {                                           \
    TS();                                                               \
    LOG(fmt, ##__VA_ARGS__);                                            \
} while (0)

/* RLOG — timestamped printf-style → RTT channel 0 AND SWO Terminal I/O. */
#define RLOG(fmt, ...) do {                                                 \
    char _tl[128];                                                          \
    int  _n = snprintf(_tl, sizeof(_tl), "[T+%7lu] " fmt "\r\n",           \
                       (unsigned long)HAL_GetTick(), ##__VA_ARGS__);        \
    if (_n > 0) SEGGER_RTT_Write(0, _tl, (unsigned)_n);                   \
    if (_n > 0) printf("%s", _tl);                                         \
} while (0)

/* RTT_TS — timestamped line to RTT channel 0 AND SWO Terminal I/O. */
#define RTT_TS(msg) do {                                                    \
    char _ts[16];                                                           \
    int _n = snprintf(_ts, sizeof(_ts), "[T+%7lu] ",                       \
                      (unsigned long)HAL_GetTick());                        \
    if (_n > 0) SEGGER_RTT_Write(0, _ts, (unsigned)_n);                   \
    SEGGER_RTT_WriteString(0, (msg));                                       \
    printf("[T+%7lu] %s", (unsigned long)HAL_GetTick(), (msg));            \
} while (0)

#ifdef __cplusplus
}
#endif

#endif /* LOG_MUTEX_H */
