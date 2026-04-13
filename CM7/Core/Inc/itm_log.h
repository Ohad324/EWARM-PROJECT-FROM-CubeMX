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

/* Combined ITM + RTT write — string literal only, no format args, no printf */
#define STAGE(msg)                           \
    do {                                     \
        _itm_str(msg "\n");                  \
        SEGGER_RTT_WriteString(0, msg "\n"); \
    } while (0)
