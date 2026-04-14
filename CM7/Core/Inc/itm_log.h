/**
 * @file    itm_log.h
 * @brief   Zero-overhead stage tracing via ITM + SEGGER RTT simultaneously.
 *
 * TWO parallel paths, one call:
 *
 *   ITM  → ITM_SendChar() from core_cm7.h — writes to ITM stimulus PORT[0].
 *           Same SWO pin used by IAR Terminal I/O. No printf, no heap.
 *
 *   RTT  → SEGGER_RTT_WriteString() — memcpy into SRAM ring buffer.
 *           Channel 0 is NO_BLOCK_SKIP (SEGGER_RTT_Conf.h). Zero latency.
 *
 * USAGE:
 *   STAGE("DFSDM1 PASS");
 *   STAGE("SD3 FAIL");
 */

#pragma once

#include "core_cm7.h"    /* ITM_SendChar() */
#include "SEGGER_RTT.h"  /* SEGGER_RTT_WriteString() */

static inline void _itm_str(const char *s)
{
    while (*s) ITM_SendChar((uint32_t)*s++);
}

/* Send a uint32_t as decimal digits to ITM port 0, followed by newline.
 * No printf, no heap, no RLOG — pure ITM. */
static inline void _itm_u32(uint32_t v)
{
    char buf[12];
    int  i = 11;
    buf[i] = '\n';
    if (v == 0u) { buf[--i] = '0'; }
    else         { while (v) { buf[--i] = '0' + (int)(v % 10u); v /= 10u; } }
    while (i <= 11) ITM_SendChar((uint32_t)(uint8_t)buf[i++]);
}

/* Combined ITM + RTT write — string literal only, no format args, no printf */
#define STAGE(msg)                           \
    do {                                     \
        _itm_str(msg "\n");                  \
        SEGGER_RTT_WriteString(0, msg "\n"); \
    } while (0)

/* ITM-only: print label + DWT cycle counter to Terminal I/O.
 * Example output in Terminal I/O:  "REC START cyc=1234567\n"
 * Usage:  STAGE_CYCLES("REC START");
 * Prerequisite: DWT_CTRL.CYCCNTENA must be set (done in main.c or here). */
#ifndef DWT_CTRL_CYCCNTENA_Msk
#define DWT_CTRL_CYCCNTENA_Msk (1UL << 0)
#endif
static inline void _itm_cycles_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT       = 0u;
    DWT->CTRL        |= DWT_CTRL_CYCCNTENA_Msk;
}

#define STAGE_CYCLES(label)          \
    do {                             \
        _itm_str(label " cyc=");     \
        _itm_u32(DWT->CYCCNT);       \
    } while (0)
