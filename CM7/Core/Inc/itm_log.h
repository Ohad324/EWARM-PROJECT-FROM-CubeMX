/**
 * @file    itm_log.h
 * @brief   Pipeline tracing via ITM PORT[0] (Terminal I/O) + SEGGER RTT.
 *
 * ITM_STAGE(token) — writes a short ASCII label + cycle count to PORT[0].
 * IAR Terminal I/O reads PORT[0] as ASCII text and displays it directly.
 *
 * Output format in Terminal I/O (one line per event):
 *   [SD1:open] cyc=00A3F200
 *   [SD3:hdr ] cyc=00A4C810
 *   [PIPE:OK ] cyc=1FA3D120
 *
 * PORT[0] — Terminal I/O ASCII text (ITM_STAGE + STAGE_CYCLES)
 *
 * USAGE:
 *   ITM_STAGE(ITM_SD1_OPEN);
 *   STAGE("SD3 FAIL");          -- RTT only, unchanged
 *   STAGE_CYCLES("REC START");  -- PORT[0] timing label, unchanged
 */

#pragma once

#include "core_cm7.h"    /* ITM struct */
#include "SEGGER_RTT.h"  /* SEGGER_RTT_WriteString() */

/* ── Low-level PORT[0] helpers ───────────────────────────────────────────── */

static inline void _itm_str(const char *s)
{
    if (((ITM->TCR & ITM_TCR_ITMENA_Msk) == 0UL) ||
        ((ITM->TER & 1UL               ) == 0UL))
        return;
    while (*s) {
        while (ITM->PORT[0U].u32 == 0UL) {}   /* wait for FIFO slot */
        ITM->PORT[0U].u8 = (uint8_t)*s++;
    }
}

static inline void _itm_u32hex(uint32_t v)
{
    /* Write 8 hex digits + newline to PORT[0] */
    static const char hex[] = "0123456789ABCDEF";
    char buf[10];
    buf[0] = hex[(v >> 28) & 0xF];
    buf[1] = hex[(v >> 24) & 0xF];
    buf[2] = hex[(v >> 20) & 0xF];
    buf[3] = hex[(v >> 16) & 0xF];
    buf[4] = hex[(v >> 12) & 0xF];
    buf[5] = hex[(v >>  8) & 0xF];
    buf[6] = hex[(v >>  4) & 0xF];
    buf[7] = hex[(v >>  0) & 0xF];
    buf[8] = '\n';
    buf[9] = '\0';
    _itm_str(buf);
}

static inline void _itm_u32(uint32_t v)
{
    char buf[12];
    int  i = 11;
    buf[i] = '\n';
    if (v == 0u) { buf[--i] = '0'; }
    else         { while (v) { buf[--i] = '0' + (int)(v % 10u); v /= 10u; } }
    while (i <= 11) {
        while (ITM->PORT[0U].u32 == 0UL) {}
        ITM->PORT[0U].u8 = (uint8_t)buf[i++];
    }
}

/* ── DWT cycle counter enable ────────────────────────────────────────────── */
#ifndef DWT_CTRL_CYCCNTENA_Msk
#define DWT_CTRL_CYCCNTENA_Msk (1UL << 0)
#endif

static inline void _itm_cycles_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT       = 0u;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

/* ── ITM_STAGE — ASCII label + hex cycle count to Terminal I/O ───────────
 * Each token maps to a fixed 10-char label (bracketed, space-padded).
 * Output: "[label   ] cyc=XXXXXXXX\n"
 * Safe no-op when ITM disabled (TCR/TER check in _itm_str). */
typedef enum
{
    /* Init sequence */
    ITM_INIT_MPU_DONE,
    ITM_INIT_CLOCKS_DONE,
    ITM_INIT_UART8_DONE,
    ITM_INIT_DFSDM_DONE,
    ITM_INIT_BLEuart_DONE,
    ITM_INIT_VOICEREC_DONE,
    ITM_INIT_AUDIOSD_DONE,
    ITM_INIT_TASK_UART,
    ITM_INIT_TASK_CMDHANDLER,
    ITM_INIT_TASK_VOICEREC,
    ITM_INIT_TASK_SDWRITE,
    ITM_INIT_TASK_RTTLOG,
    ITM_INIT_KERNEL_START,

    /* Pipeline success */
    ITM_SD1_OPEN,
    ITM_SD2_ALLOC,
    ITM_SD3_HDR,
    ITM_SD4_PCM_START,
    ITM_SD5_CLOSED,
    ITM_UART_SEND,
    ITM_STREAM1_HDR,
    ITM_STREAM2_READY,
    ITM_STREAM3_DONE,
    ITM_PIPELINE_DONE,

    /* Pipeline errors */
    ITM_ERR_SD1_OPEN,
    ITM_ERR_SD4_PCM,
    ITM_ERR_SD5_CLOSE,
    ITM_ERR_UART_STREAM,
    ITM_ERR_STREAM2_TMO,
    ITM_ERR_STREAM3,

    ITM_TOKEN_COUNT
} PipelineToken_t;

/* Label table — 12 chars each, null-terminated, shown in Terminal I/O */
static const char * const s_itmLabels[ITM_TOKEN_COUNT] = {
    /* Init */
    "[MPU:done] ",
    "[CLK:done] ",
    "[UART8:ok] ",
    "[DFSDM:ok] ",
    "[BLE:done] ",
    "[VREC:done]",
    "[ASD:done] ",
    "[T:UART   ]",
    "[T:CMDHND ]",
    "[T:VREC   ]",
    "[T:SDWRITE]",
    "[T:RTTLOG ]",
    "[KERNEL:go]",
    /* Pipeline OK */
    "[SD1:open ]",
    "[SD2:alloc]",
    "[SD3:hdr  ]",
    "[SD4:pcm  ]",
    "[SD5:close]",
    "[UART:send]",
    "[STR1:hdr ]",
    "[STR2:rdy ]",
    "[STR3:done]",
    "[PIPE:OK  ]",
    /* Pipeline ERR */
    "[ERR:SD1  ]",
    "[ERR:SD4  ]",
    "[ERR:SD5  ]",
    "[ERR:UART ]",
    "[ERR:STR2T]",
    "[ERR:STR3 ]",
};

/* ─────────────────────────────────────────────────────────────────────────
 * RELEASE_BUILD compiles ITM macros out completely.
 * Otherwise (debug): ITM_STAGE writes label+cycle count to PORT[0],
 *                    STAGE writes RTT, STAGE_CYCLES writes PORT[0].
 * ─────────────────────────────────────────────────────────────────────── */
#ifdef RELEASE_BUILD
#  define ITM_STAGE(token)         do { (void)(token); } while (0)
#  define STAGE(msg)               do { (void)(msg);   } while (0)
#  define STAGE_CYCLES(label)      do { (void)(label); } while (0)
#else
#define ITM_STAGE(token)                                        \
    do {                                                        \
        if (((ITM->TCR & ITM_TCR_ITMENA_Msk) != 0UL) &&       \
            ((ITM->TER & 1UL               ) != 0UL)) {        \
            _itm_str(s_itmLabels[(int)(token)]);                \
            _itm_str(" cyc=");                                  \
            _itm_u32hex(DWT->CYCCNT);                          \
        }                                                       \
    } while (0)

/* ── STAGE — RTT only ────────────────────────────────────────────────────── */
#define STAGE(msg)   SEGGER_RTT_WriteString(0, msg "\n")

/* ── STAGE_CYCLES — PORT[0] label + decimal cycle count ─────────────────── */
#define STAGE_CYCLES(label)          \
    do {                             \
        _itm_str(label " cyc=");     \
        _itm_u32(DWT->CYCCNT);       \
    } while (0)
#endif /* RELEASE_BUILD */
