/**
 * @file  rtos_trace.h
 * @brief Lightweight FreeRTOS event tracer — ring buffer + RTT channel 1 drain.
 *
 * USAGE
 * -----
 * 1. FreeRTOSConfig.h includes this header and defines the trace hook macros
 *    (see the USER CODE Defines block).
 * 2. Call RtosTrace_Init() before vTaskStartScheduler() / osKernelStart().
 * 3. Create a FreeRTOS task that runs RtosTrace_DrainTask(), priority 1.
 * 4. In J-Link RTT Viewer open Terminal 1 to see RTOS events.
 *    Terminal 0 continues to carry your application LOG() output.
 *
 * RING BUFFER
 * -----------
 * 1024 entries × 16 bytes = 16 KB in internal SRAM.
 * Lock-free multi-producer (LDREX/STREX) — safe from task, ISR, and FreeRTOS
 * critical sections.  The drain task runs every 5 ms and empties the buffer
 * before it can overflow at typical event rates.  Overflow events are counted
 * and reported on Terminal 1.
 */
#ifndef RTOS_TRACE_H
#define RTOS_TRACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event codes ─────────────────────────────────────────────────────────── */
#define TRC_Q_SEND           0x01u  /* Queue / semaphore / mutex  send (task)   */
#define TRC_Q_SEND_FAIL      0x02u  /* Send failed (full / semaphore overflow)  */
#define TRC_Q_SEND_ISR       0x03u  /* Send from ISR  — success                */
#define TRC_Q_SEND_ISR_FAIL  0x04u  /* Send from ISR  — failed                 */
#define TRC_Q_RECV           0x05u  /* Receive / take  (task)    — success      */
#define TRC_Q_RECV_FAIL      0x06u  /* Receive failed (empty, non-blocking)     */
#define TRC_Q_RECV_ISR       0x07u  /* Receive from ISR           — success     */
#define TRC_Q_RECV_ISR_FAIL  0x08u  /* Receive from ISR           — failed      */
#define TRC_BLOCK_RECV       0x09u  /* Task about to block waiting to receive   */
#define TRC_BLOCK_SEND       0x0Au  /* Task about to block waiting to send      */
#define TRC_TASK_DELAY       0x0Bu  /* vTaskDelay() called                      */
#define TRC_TASK_DELAY_UNTIL 0x0Cu  /* vTaskDelayUntil() called                 */
#define TRC_TASK_SUSPEND     0x0Du  /* vTaskSuspend()   — which task            */
#define TRC_TASK_RESUME      0x0Eu  /* vTaskResume()    — which task            */
#define TRC_TASK_RESUME_ISR  0x0Fu  /* xTaskResumeFromISR() — which task        */
#define TRC_TASK_CREATE      0x10u  /* Task created     — which task            */
#define TRC_TASK_DELETE      0x11u  /* Task deleted     — which task            */
#define TRC_TIMER_EXPIRE     0x12u  /* SW timer callback executed               */

/* ── Public API ─────────────────────────────────────────────────────────── */

/**
 * Record a queue / semaphore / mutex event.
 * Internally looks up the current task name — safe from any execution context.
 */
void rtos_trace_queue_event(unsigned char type, void* handle);

/**
 * Record a named task or timer event.
 * name must point to a static string (e.g. TCB pcTaskName or timer pcTimerName).
 * Pass NULL to look up the current task name automatically.
 */
void rtos_trace_task_event(unsigned char type, const char* name);

/** Configure DWT and RTT channel 1.  Call once before the scheduler starts. */
void RtosTrace_Init(void);

/** FreeRTOS task body.  Create at priority 1 (lowest application priority). */
void RtosTrace_DrainTask(void* arg);

/**
 * Record one switch-in for the given task.
 * Called from traceTASK_SWITCHED_IN (PendSV context — no FreeRTOS API allowed).
 * Uses only a pointer-compare loop and a counter increment — ISR-safe.
 * name must point to a static string (TCB pcTaskName).
 */
void rtos_trace_switched_in(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* RTOS_TRACE_H */
