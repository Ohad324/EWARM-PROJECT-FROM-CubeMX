/**
 * @file    log_mutex.h
 * @brief   Deferred logging — all tasks post to xLogQueue, RTTLogTask prints.
 *
 * Architecture (B-011 fix):
 *   Any task calls Log_ToQueue("LABEL", value) — captures HAL_GetTick() ms,
 *   copies a 4-byte pointer, posts 12-byte entry to xLogQueue (timeout=100 ms
 *   when scheduler running, 0 before).  If queue is full and timeout expires,
 *   g_logDropped is incremented; RTTLogTask reports "LOG_DROPPED" periodically.
 *
 *   RTTLogTask (priority 1) drains the queue and calls SEGGER_RTT_Write() only.
 *   No printf/ITM in the runtime log path — ITM spin-wait cannot occur.
 *
 * ITM / SWO is NOT used for runtime logging (printf removed from RTTLogTask).
 * It is reserved for STAGE_CYCLES() timing measurements only (itm_log.h).
 * Pre-RTOS LOG/LOG_TS macros still call printf — safe before vTaskStartScheduler.
 *
 * RLOG(msg, val)  — post string literal + one value to the log queue
 * RLOG0(msg)      — same with val=0 (no numeric context needed)
 * LOG / LOG_TS    — legacy SWO-only, kept for early-boot use before RTOS starts
 * RTT_TS          — fixed-string RTT write, kept for ISR-safe one-shot notes
 */

#ifndef LOG_MUTEX_H
#define LOG_MUTEX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>        /* printf — used by LOG / LOG_TS (pre-RTOS boot only) */
#include <stdint.h>
#include "stm32h7xx_hal.h"  /* HAL_GetTick() */
#include "SEGGER_RTT.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"
#include "voice_recorder.h"  /* LogMsg_t */

/* ── xLogQueue — created in main.c, shared across all modules ───────────── */
extern QueueHandle_t xLogQueue;

/* ── g_logDropped — incremented when queue is full, read+reset by RTTLogTask */
extern volatile uint32_t g_logDropped;

/* ── Log_ToQueue — the ONLY write path for all runtime logging ──────────── *
 *                                                                            *
 *  • Captures HAL_GetTick() ms immediately.                                  *
 *  • Copies a 4-byte pointer to a flash string literal — not the string.    *
 *  • Uses pdMS_TO_TICKS(100) timeout after scheduler start.                 *
 *  • If queue still full after timeout, increments g_logDropped (no block). */
static inline void Log_ToQueue(const char *pcMsg, uint32_t val)
{
    /* Never call from ISR — use SEGGER_RTT_WriteString() there instead.
     * Guard: if somehow called from ISR, drop silently rather than assert. */
    if (SCB->ICSR & SCB_ICSR_VECTACTIVE_Msk) { g_logDropped++; return; }

    LogMsg_t e;
    e.timestamp = HAL_GetTick();  /* milliseconds since boot — no overflow for 49 days */
    e.pcMsg     = pcMsg;
    e.val       = val;
    if (xLogQueue != NULL)
    {
        TickType_t waitTicks = 0u;
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
            waitTicks = pdMS_TO_TICKS(100u);
        if (xQueueSend(xLogQueue, &e, waitTicks) != pdTRUE)
            g_logDropped++;   /* queue still full — count the drop, never block further */
    }
}

/* ── RLOG — timestamped log entry via queue ─────────────────────────────── *
 * msg  : string literal (pointer copied to flash, no heap, no snprintf)     *
 * val  : optional uint32_t value; cast explicitly for signed/pointer types   */
#define RLOG(msg, val)   Log_ToQueue((msg), (uint32_t)(val))

/* Zero-value shorthand for messages that carry no numeric context */
#define RLOG0(msg)       Log_ToQueue((msg), 0u)

/* ── Legacy macros — for early-boot code before RTOS starts ─────────────── *
 * These call printf() directly (SWO Terminal I/O).  Safe only before        *
 * vTaskStartScheduler() when no real-time tasks are running.                 */

/* LOG — printf-style to Terminal I/O AND RTT (no timestamp). */
#define LOG(fmt, ...) do {                                                  \
    char _lb[256];                                                          \
    int  _ln = snprintf(_lb, sizeof(_lb), fmt, ##__VA_ARGS__);             \
    if (_ln > 0) SEGGER_RTT_Write(0, _lb, (unsigned)_ln);                  \
    printf(fmt, ##__VA_ARGS__);                                             \
} while (0)

/* LOG_TS — timestamped printf-style to Terminal I/O (pre-RTOS only). */
#define LOG_TS(fmt, ...) do {                           \
    printf("[T+%7lu] ", (unsigned long)HAL_GetTick());  \
    LOG(fmt, ##__VA_ARGS__);                            \
} while (0)

/* RTT_TS — timestamped fixed string to RTT + Terminal I/O. */
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
