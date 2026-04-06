/**
 * @file  rtos_trace.c
 * @brief FreeRTOS event tracer implementation.
 *
 * Ring buffer: 1024 entries × 16 bytes = 16 KB in .bss (internal SRAM).
 * Lock-free write via ARM LDREX/STREX exclusive access.
 * Drain task: formats events and writes to RTT up-buffer 1 ("RTOS" terminal).
 */

#include "rtos_trace.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* SEGGER RTT */
#include "SEGGER_RTT.h"
#include "stm32h7xx_hal.h"  /* HAL_GetTick() for timestamps */

/* CMSIS (DWT, CoreDebug) */
#include "stm32h7xx.h"

#include <stdint.h>
#include <stdio.h>

/* ── Configuration ───────────────────────────────────────────────────────── */
#define TRACE_BUF_ENTRIES   1024u        /* must be a power of 2 */
#define TRACE_RTT_CHANNEL   1u           /* RTT up-buffer index   */
#define TRACE_RTT_BUF_BYTES 8192u        /* RTT host-side buffer  */
#define DRAIN_PERIOD_MS     5u           /* drain wakeup interval */
#define CPU_CLK_HZ          480000UL     /* 480 MHz Cortex-M7     */
#define WATERMARK_PERIOD_MS 200u         /* stack HWM report interval */
#define MAX_TASK_COUNT      16u          /* upper bound for system state */

/* ── Ring buffer record (16 bytes) ──────────────────────────────────────── */
typedef struct
{
    uint32_t    ts_cyc;     /* DWT->CYCCNT at capture time              */
    uint32_t    handle;     /* queue / semaphore handle address (or 0)  */
    const char* task_name;  /* pointer into TCB pcTaskName[] (static)   */
    uint8_t     event;      /* TRC_xxx code                             */
    uint8_t     _pad[3];
} rtos_trace_rec_t;

/* ── Static storage ──────────────────────────────────────────────────────── */
static rtos_trace_rec_t s_buf[TRACE_BUF_ENTRIES];   /* 16 KB in .bss */

/* Ever-incrementing counters; wrap-around via masking */
static volatile uint32_t s_write_idx = 0u;
static volatile uint32_t s_drain_idx = 0u;
static volatile uint32_t s_dropped   = 0u;

/* ── Per-task run counters ────────────────────────────────────────────────── */
typedef struct
{
    const char* name;   /* pointer into TCB pcTaskName[] — static, never freed */
    uint32_t    count;  /* switches-in since last report */
} task_run_count_t;

static task_run_count_t  s_run_counts[MAX_TASK_COUNT];
static volatile uint32_t s_n_tasks = 0u;

/* RTT buffer for channel 1 (host-side) */
static uint8_t s_rtt_buf[TRACE_RTT_BUF_BYTES];

/* ── Lock-free slot claim ────────────────────────────────────────────────── */
/*
 * Uses ARM exclusive access (LDREX/STREX) so it is safe from task context,
 * ISR context, AND FreeRTOS critical sections (where normal mutexes cannot
 * be used).  The actual data write happens after the slot is owned — no
 * further synchronisation is needed because each producer owns a unique slot.
 */
static uint32_t claim_slot(void)
{
    uint32_t w, next;
    do {
        w = __LDREXW(&s_write_idx);
        if ((w - s_drain_idx) >= TRACE_BUF_ENTRIES) {
            /* Buffer full: count overflow and give up */
            __CLREX();
            s_dropped++;
            return TRACE_BUF_ENTRIES;   /* sentinel: discard */
        }
        next = w + 1u;
    } while (__STREXW(next, &s_write_idx) != 0u);

    return w & (TRACE_BUF_ENTRIES - 1u);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void rtos_trace_queue_event(unsigned char type, void* handle)
{
    uint32_t idx = claim_slot();
    if (idx == TRACE_BUF_ENTRIES) return;

    /* Get current task name.  Works from task, ISR, or critical-section context
     * because xTaskGetCurrentTaskHandle() is just a read of pxCurrentTCB. */
    TaskHandle_t h    = xTaskGetCurrentTaskHandle();
    const char*  name = (h != NULL) ? pcTaskGetName(h) : "pre-sched";

    s_buf[idx].ts_cyc    = DWT->CYCCNT;
    s_buf[idx].handle    = (uint32_t)handle;
    s_buf[idx].task_name = name;
    s_buf[idx].event     = type;
}

void rtos_trace_task_event(unsigned char type, const char* name)
{
    uint32_t idx = claim_slot();
    if (idx == TRACE_BUF_ENTRIES) return;

    /* If caller passed NULL, look up the current task name */
    if (name == NULL) {
        TaskHandle_t h = xTaskGetCurrentTaskHandle();
        name = (h != NULL) ? pcTaskGetName(h) : "pre-sched";
    }

    s_buf[idx].ts_cyc    = DWT->CYCCNT;
    s_buf[idx].handle    = 0u;
    s_buf[idx].task_name = name;
    s_buf[idx].event     = type;
}

void rtos_trace_switched_in(const char* name)
{
    if (name == NULL) return;

    uint32_t n = s_n_tasks;

    /* Fast path: task already registered — pointer compare only, no strcmp */
    for (uint32_t i = 0u; i < n; i++) {
        if (s_run_counts[i].name == name) {
            s_run_counts[i].count++;
            return;
        }
    }

    /* New task: register it.  Write name+count before exposing via s_n_tasks
     * so the drain task never sees a half-initialised entry. */
    if (n < MAX_TASK_COUNT) {
        s_run_counts[n].name  = name;
        s_run_counts[n].count = 1u;
        s_n_tasks = n + 1u;
    }
}

void RtosTrace_Init(void)
{
    /* Enable DWT cycle counter (may already be on from timing_log / DWT_SNAP) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;

    /* Ensure the RTT control block is initialised (sets up channel 0) */
    SEGGER_RTT_Init();

    /* Configure channel 1 directly — SEGGER_RTT_ConfigUpBuffer is not present
     * in this project's minimal RTT build.  The _SEGGER_RTT control block is
     * exported from SEGGER_RTT.c, and J-Link reads channel descriptors from it. */
    _SEGGER_RTT.aUp[TRACE_RTT_CHANNEL].sName        = "RTOS";
    _SEGGER_RTT.aUp[TRACE_RTT_CHANNEL].pBuffer      = (char*)s_rtt_buf;
    _SEGGER_RTT.aUp[TRACE_RTT_CHANNEL].SizeOfBuffer = sizeof(s_rtt_buf);
    _SEGGER_RTT.aUp[TRACE_RTT_CHANNEL].WrOff        = 0u;
    _SEGGER_RTT.aUp[TRACE_RTT_CHANNEL].RdOff        = 0u;
    _SEGGER_RTT.aUp[TRACE_RTT_CHANNEL].Flags        = SEGGER_RTT_MODE_NO_BLOCK_SKIP;
}

/* ── Drain task ──────────────────────────────────────────────────────────── */

static const char* const s_evt_names[] =
{
    "?",                /* 0x00 unused             */
    "Q_SEND",           /* 0x01 send from task     */
    "Q_SEND_FAIL",      /* 0x02 send failed        */
    "Q_SEND_ISR",       /* 0x03 send from ISR      */
    "Q_SEND_ISR_FAIL",  /* 0x04 ISR send failed    */
    "Q_RECV",           /* 0x05 receive from task  */
    "Q_RECV_FAIL",      /* 0x06 receive failed     */
    "Q_RECV_ISR",       /* 0x07 receive from ISR   */
    "Q_RECV_ISR_FAIL",  /* 0x08 ISR recv failed    */
    "BLOCK_RECV",       /* 0x09 blocking wait recv */
    "BLOCK_SEND",       /* 0x0A blocking wait send */
    "TASK_DELAY",       /* 0x0B vTaskDelay         */
    "TASK_DELAY_UNTIL", /* 0x0C vTaskDelayUntil    */
    "TASK_SUSPEND",     /* 0x0D suspend            */
    "TASK_RESUME",      /* 0x0E resume from task   */
    "TASK_RESUME_ISR",  /* 0x0F resume from ISR    */
    "TASK_CREATE",      /* 0x10 task created       */
    "TASK_DELETE",      /* 0x11 task deleted       */
    "TIMER_EXPIRE",     /* 0x12 timer callback     */
};
#define EVT_NAMES_COUNT  ((uint32_t)(sizeof(s_evt_names) / sizeof(s_evt_names[0])))

/* ── Stack overflow hook (requires configCHECK_FOR_STACK_OVERFLOW 2) ──────── */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    /* Write directly — stack is corrupt, snprintf is unsafe */
    SEGGER_RTT_WriteString(0, "[FATAL] Stack overflow: task=");
    SEGGER_RTT_WriteString(0, pcTaskName ? pcTaskName : "?");
    SEGGER_RTT_WriteString(0, "\n");
    for (;;) {}
}

/* ── Periodic run-count report ───────────────────────────────────────────── */
static void emit_run_counts(void)
{
    uint32_t n = s_n_tasks;
    char     line[72];

    {
        int len = snprintf(line, sizeof(line),
            "[T+%7lu] [RUNS] -- switches per %u ms --\n",
            (unsigned long)HAL_GetTick(), WATERMARK_PERIOD_MS);
        if (len > 0)
            SEGGER_RTT_Write(0u, line, (unsigned)len);
    }

    for (uint32_t i = 0u; i < n; i++) {
        uint32_t cnt          = s_run_counts[i].count;
        s_run_counts[i].count = 0u;   /* reset: next report shows delta only */
        int len = snprintf(line, sizeof(line),
            "[T+%7lu] [RUNS] %-20s  %lu\n",
            (unsigned long)HAL_GetTick(),
            s_run_counts[i].name ? s_run_counts[i].name : "?",
            (unsigned long)cnt);
        if (len > 0)
            SEGGER_RTT_Write(0u, line, (unsigned)len);
    }
}

/* ── Periodic stack watermark report ─────────────────────────────────────── */
static void emit_stack_watermarks(void)
{
    static TaskStatus_t  info[MAX_TASK_COUNT];  /* static: off the stack */
    char                 line[72];
    UBaseType_t          n;

    n = uxTaskGetSystemState(info, MAX_TASK_COUNT, NULL);

    {
        int len = snprintf(line, sizeof(line),
            "[T+%7lu] [STACK] -- HWM heap_free=%lu B --\n",
            (unsigned long)HAL_GetTick(),
            (unsigned long)xPortGetFreeHeapSize());
        if (len > 0)
            SEGGER_RTT_Write(0u, line, (unsigned)len);
    }

    for (UBaseType_t i = 0u; i < n; i++) {
        int len = snprintf(line, sizeof(line),
            "[T+%7lu] [STACK] %-20s  hwm=%4u words\n",
            (unsigned long)HAL_GetTick(),
            info[i].pcTaskName,
            (unsigned)info[i].usStackHighWaterMark);
        if (len > 0)
            SEGGER_RTT_Write(0u, line, (unsigned)len);
    }
}

void RtosTrace_DrainTask(void* arg)
{
    (void)arg;
    char     line[100];
    uint32_t last_dropped  = 0u;
    uint32_t drain_tick    = 0u;
    const uint32_t wm_every = WATERMARK_PERIOD_MS / DRAIN_PERIOD_MS;  /* 200 cycles */

    /* One-shot early report: give tasks 50 ms to initialise then capture
     * initial stack usage before any crash can occur. */
    vTaskDelay(pdMS_TO_TICKS(50u));
    emit_stack_watermarks();

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(DRAIN_PERIOD_MS));

        /* ── Report overflows ─────────────────────────────────────────────── */
        {
            uint32_t d = s_dropped;
            if (d != last_dropped) {
                int n = snprintf(line, sizeof(line),
                    "[RTOS] *** OVERFLOW — %lu events lost ***\n",
                    (unsigned long)(d - last_dropped));
                if (n > 0)
                    SEGGER_RTT_Write(TRACE_RTT_CHANNEL, line, (unsigned)n);
                last_dropped = d;
            }
        }

        /* ── Periodic stack watermark + run-count report ─────────────────── */
        if (++drain_tick >= wm_every) {
            drain_tick = 0u;
            emit_stack_watermarks();
            emit_run_counts();
        }

        /* ── Drain all pending entries ────────────────────────────────────── */
        while (s_drain_idx != s_write_idx)
        {
            uint32_t idx = s_drain_idx & (TRACE_BUF_ENTRIES - 1u);

            /* Snapshot the entry before advancing the drain index */
            rtos_trace_rec_t r = s_buf[idx];
            s_drain_idx++;

            /* Convert DWT cycles to milliseconds + microsecond fraction */
            uint32_t ms   = r.ts_cyc / CPU_CLK_HZ;
            uint32_t frac = (r.ts_cyc % CPU_CLK_HZ) / (CPU_CLK_HZ / 1000u);  /* 0-999 */

            const char* evname = (r.event < EVT_NAMES_COUNT)
                                 ? s_evt_names[r.event] : "???";
            const char* task   = (r.task_name != NULL) ? r.task_name : "?";

            int n;
            if (r.handle != 0u) {
                /* Queue / semaphore event — print handle address */
                n = snprintf(line, sizeof(line),
                    "[RTOS] %7lu.%03lums  %-18s  h=0x%08lX  task=%s\n",
                    (unsigned long)ms,
                    (unsigned long)frac,
                    evname,
                    (unsigned long)r.handle,
                    task);
            } else {
                /* Task / timer event — no handle */
                n = snprintf(line, sizeof(line),
                    "[RTOS] %7lu.%03lums  %-18s  task=%s\n",
                    (unsigned long)ms,
                    (unsigned long)frac,
                    evname,
                    task);
            }

            if (n > 0)
                SEGGER_RTT_Write(TRACE_RTT_CHANNEL, line, (unsigned)n);
        }
    }
}
