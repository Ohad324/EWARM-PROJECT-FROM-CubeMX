/**
 * @file    rtt_log_task.c
 * @brief   RTTLogTask — sole consumer of xLogQueue, drives SD-DETECT LED.
 *
 * Architecture:
 *   Every task calls Log_ToQueue() (log_mutex.h) which posts a LogMsg_t
 *   {timestamp, pcMsg pointer, val} to xLogQueue with a non-ISR-safe send.
 *   RTTLogTask at priority 1 (lowest) drains the queue and writes formatted
 *   output to BOTH:
 *     - SEGGER RTT channel 0  (non-blocking, NO_BLOCK_SKIP)
 *     - IAR Terminal I/O      (printf → ITM_SendChar, spin-wait)
 *
 *   printf/ITM spin-wait is SAFE here because RTTLogTask runs at priority 1.
 *   Any task with priority >= 2 preempts it instantly, so the spin only burns
 *   idle CPU cycles — it cannot starve real-time tasks.
 *
 * Drop reporting:
 *   If xLogQueue fills, Log_ToQueue increments g_logDropped (volatile).
 *   RTTLogTask reports and resets the counter every 1 second (5 × 200 ms).
 */

#include "rtt_log_task.h"
#include "main.h"             /* DFSDM_Debug_Hub_t, g_dbg              */
#include "log_mutex.h"        /* xLogQueue, LogMsg_t, g_logDropped        */
#include "voice_recorder.h"   /* voiceRecTaskHandle, g_sdFreeKB, g_AudioHealth, VoiceRec_GetState */
#include "audio_sd.h"         /* g_UartHealth                             */
#include "stm32h7xx_hal.h"    /* HAL_GPIO_ReadPin/WritePin, HAL_GetTick   */
#include "SEGGER_RTT.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <stdio.h>            /* printf, snprintf                          */

/* SD_DETECT: PI8 active-low.  LED2: PI13 mirrors card-present state. */
#define SD_DETECT_PORT   GPIOI
#define SD_DETECT_PIN    GPIO_PIN_8
#define LED2_PORT        GPIOI
#define LED2_PIN         GPIO_PIN_13

/* Emit one formatted string to RTT channel 0. */
static inline void emit(const char *buf, int n)
{
    if (n <= 0) return;
    SEGGER_RTT_Write(0, buf, (unsigned)n);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  DFSDM_BootCheck — one-shot self-test printed at startup
 *
 *  Prints 12 verification items matching the hardware design intent:
 *    LR pin, channel, edge, SPICKSEL, CHEN, DFSDMEN, CKOUTDIV,
 *    RCSEL, filter mode, DTRBS, DMA path, false-DC guard.
 *  Each line: [OK] confirmed correct / [FAIL] mismatch / [PEND] needs recording.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void DFSDM_BootCheck(void)
{
    char b[160];
    int  n;

    /* Refresh all g_dbg fields from live registers */
    g_dbg.ch0_cfg1     = DFSDM1_Channel1->CHCFGR1;  /* Ch1 = active data channel (SAI4 bridge D1, LR=HIGH mic) */
    g_dbg.ch0_cfg2     = DFSDM1_Channel1->CHCFGR2;
    g_dbg.flt0_cr1     = DFSDM1_Filter0->FLTCR1;
    g_dbg.flt0_cr2     = DFSDM1_Filter0->FLTCR2;
    g_dbg.flt0_fcr     = DFSDM1_Filter0->FLTFCR;
    g_dbg.flt0_isr     = DFSDM1_Filter0->FLTISR;
    g_dbg.sai4_pdm     = SAI4->PDMCR;
    g_dbg.dma_cr       = DMA1_Stream1->CR;
    g_dbg.dma_ndtr     = DMA1_Stream1->NDTR;
    g_dbg.dma_m0ar     = DMA1_Stream1->M0AR;
    g_dbg.sitp         = g_dbg.ch0_cfg1 & 0x3u;
    g_dbg.spicksel     = (g_dbg.ch0_cfg1 >> 2u) & 0x3u;
    g_dbg.dtrbs        = (g_dbg.ch0_cfg2 >> 3u) & 0x1Fu;
    g_dbg.last_raw     = (uint32_t)DFSDM1_Filter0->FLTRDATAR;
    g_dbg.last_val     = (int32_t)((int32_t)g_dbg.last_raw >> 8);
    g_dbg.uptime_ticks = 0u;   /* boot-check snapshot — uptime not meaningful yet */

    /* Read from struct — single consistent snapshot for all checks below */
    uint32_t ch0cfg1   = g_dbg.ch0_cfg1;
    uint32_t ch0cfg2   = g_dbg.ch0_cfg2;
    uint32_t fltcr1    = g_dbg.flt0_cr1;
    uint32_t fltfcr    = g_dbg.flt0_fcr;
    uint32_t sai4pdmcr = g_dbg.sai4_pdm;
    uint32_t ndtr      = g_dbg.dma_ndtr;

    /* Decode individual fields */
    uint8_t  dfsdmen  = (uint8_t)((ch0cfg1 >> 31) & 0x1u);
    uint8_t  ckoutdiv = (uint8_t)((ch0cfg1 >> 16) & 0x7Fu);
    uint8_t  chen     = (uint8_t)((ch0cfg1 >>  7) & 0x1u);
    uint8_t  spicksel = (uint8_t)((ch0cfg1 >>  2) & 0x3u);
    uint8_t  sitp     = (uint8_t)( ch0cfg1        & 0x3u);
    uint8_t  dtrbs    = (uint8_t)((ch0cfg2 >>  3) & 0x1Fu);
    uint8_t  rcsel    = (uint8_t)((fltcr1  >> 24) & 0xFu);
    uint8_t  ford     = (uint8_t)((fltfcr  >> 29) & 0x7u);
    uint16_t osr      = (uint16_t)(((fltfcr >> 16) & 0x1FFu) + 1u);
    uint8_t  rdmaen   = (uint8_t)((fltcr1  >> 21) & 0x1u);
    uint8_t  dfen     = (uint8_t)( fltcr1         & 0x1u);

#define PF(cond) ((cond) ? "OK  " : "FAIL")
#define RTT(buf, len) SEGGER_RTT_Write(0u, (buf), (unsigned)(len))

    n = snprintf(b, sizeof(b), "\r\n[BOOT] ===== DFSDM Audio Pipeline Self-Check =====\r\n"); RTT(b, n);

    /* 1. LR hardware — fixed by schematic (SB43=OPEN, R213 pullup) */
    n = snprintf(b, sizeof(b), "[BOOT]  1. LR hardware        : HIGH (RIGHT mic, SB43=OPEN, R213 pullup)  [HW-FIXED]\r\n"); RTT(b, n);

    /* 2. DFSDM channel — confirm CH1 is active (RCSEL=1 in filter) */
    n = snprintf(b, sizeof(b), "[BOOT]  2. DFSDM channel       : CH%u  (RCSEL bits[27:24])   [%s] expect 1\r\n",
                 rcsel, PF(rcsel == 1u)); RTT(b, n);

    /* 3. Edge selection — SITP=01 = falling edge (LR=HIGH mic drives data on falling CLK) */
    n = snprintf(b, sizeof(b), "[BOOT]  3. Edge (SITP)         : %s  (bits[1:0]=0x%02X)       [%s] expect 01=falling\r\n",
                 (sitp == 1u) ? "FALLING(01)" : "RISING (00)",
                 sitp, PF(sitp == 1u)); RTT(b, n);

    /* 4. SPICKSEL — must be 01 (internal CKOUT, SITP controls edge) — RM0399 p1182 */
    n = snprintf(b, sizeof(b), "[BOOT]  4. SPICKSEL            : %u%u  (bits[3:2]=0x%02X)       [%s] expect 01=CKOUT\r\n",
                 (spicksel >> 1) & 1u, spicksel & 1u,
                 spicksel, PF(spicksel == 1u)); RTT(b, n);

    /* 5. CHEN — channel enabled */
    n = snprintf(b, sizeof(b), "[BOOT]  5. CHEN (CH1 enable)   : %u  (bit7)                  [%s] expect 1\r\n",
                 chen, PF(chen == 1u)); RTT(b, n);

    /* 6. DFSDMEN — global enable */
    n = snprintf(b, sizeof(b), "[BOOT]  6. DFSDMEN (global)    : %u  (bit31)                 [%s] expect 1\r\n",
                 dfsdmen, PF(dfsdmen == 1u)); RTT(b, n);

    /* 7. CKOUTDIV — 24 → CKOUT=2.0 MHz */
    n = snprintf(b, sizeof(b), "[BOOT]  7. CKOUTDIV            : %u  (bits[22:16])           [%s] expect 24 -> 2.0 MHz\r\n",
                 ckoutdiv, PF(ckoutdiv == 24u)); RTT(b, n);

    /* 8. Filter RCSEL — Channel 1 feeds regular filter (CKOUT+falling edge, LR=HIGH mic) */
    n = snprintf(b, sizeof(b), "[BOOT]  8. RCSEL (filter->CH)  : %u  FLTCR1=0x%08lX      [%s] expect 1=CH1\r\n",
                 rcsel, (unsigned long)fltcr1, PF(rcsel == 1u)); RTT(b, n);

    /* 9. Filter mode — Sinc3, OSR=125 */
    n = snprintf(b, sizeof(b), "[BOOT]  9. Filter mode         : Sinc%u  OSR=%u  FLTFCR=0x%08lX  [%s]\r\n",
                 ford, osr, (unsigned long)fltfcr,
                 PF(ford == 3u && osr == 125u)); RTT(b, n);

    /* 10. DMA path — DFEN and RDMAEN (RDMAEN set at recording start, may be 0 here) */
    n = snprintf(b, sizeof(b), "[BOOT] 10. DMA path            : DFEN=%u RDMAEN=%u NDTR=%-5lu  [%s] NDTR moves during rec\r\n",
                 dfen, rdmaen, (unsigned long)ndtr,
                 PF(dfen == 1u)); RTT(b, n);

    /* 11. DTRBS — must be 6 (critical: 5 causes int16_t overflow -> false DC=4501) */
    n = snprintf(b, sizeof(b), "[BOOT] 11. DTRBS (shift)       : %u  CH0CFG2=0x%08lX      [%s] expect 6 (not 5!)\r\n",
                 dtrbs, (unsigned long)ch0cfg2, PF(dtrbs == 6u)); RTT(b, n);

    /* 12. False DC guard — PCM plateau 0x1195 means DTRBS=5 bug still active */
    n = snprintf(b, sizeof(b), "[BOOT] 12. False DC guard      : 0x1195 plateau=DTRBS=5 bug [PEND - verify after recording]\r\n"); RTT(b, n);

    /* SAI4 PDMCR bonus check */
    n = snprintf(b, sizeof(b), "[BOOT]     SAI4 PDMCR          : 0x%08lX                    [%s] expect 0x00000101\r\n",
                 (unsigned long)sai4pdmcr, PF(sai4pdmcr == 0x00000101u)); RTT(b, n);

    /* Summary */
    uint8_t allOk = (rcsel == 1u) && (sitp == 1u) && (spicksel == 1u) &&
                    (chen == 1u)  && (dfsdmen == 1u) && (ckoutdiv == 24u) &&
                    (ford == 3u)  && (osr == 125u)   && (dtrbs == 6u) &&
                    (dfen == 1u)  && (sai4pdmcr == 0x00000101u);
    n = snprintf(b, sizeof(b), "[BOOT] ===== %s =====\r\n\r\n",
                 allOk ? "ALL CONFIG OK - press button to record" : "*** CONFIG FAULT - check FAIL items above ***");
    RTT(b, n);

#undef RTT
#undef PF
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTTLogTask
 * ═══════════════════════════════════════════════════════════════════════════ */
void RTTLogTask(void *arg)
{
    (void)arg;
    uint32_t cycleCount = 0u;   /* 200 ms ticks — drop report every 5 cycles (1 s)  */
    uint32_t pingCount  = 0u;   /* 1 s ticks    — system health ping every 25 (5 s) */

    /* One-shot boot self-test — prints [BOOT] lines to RTT + Terminal I/O */
    DFSDM_BootCheck();

    for (;;)
    {
        /* ── Refresh g_dbg every tick (200 ms) — IAR Live Watch at 0x24000050 ─
         * Fields [0..8] updated here. Fields [9..10] (ref_lr_low/high) are
         * constant reference values written once in MX_DFSDM1_Init().
         * uptime_ticks increments each pass — proves this task is alive.       */
        g_dbg.ch0_cfg1     = DFSDM1_Channel1->CHCFGR1;  /* Ch1 = active data channel (SAI4 bridge D1, LR=HIGH mic) */
        g_dbg.ch0_cfg2     = DFSDM1_Channel1->CHCFGR2;
        g_dbg.flt0_cr1     = DFSDM1_Filter0->FLTCR1;
        g_dbg.flt0_cr2     = DFSDM1_Filter0->FLTCR2;
        g_dbg.flt0_fcr     = DFSDM1_Filter0->FLTFCR;
        g_dbg.flt0_isr     = DFSDM1_Filter0->FLTISR;
        g_dbg.sai4_pdm     = SAI4->PDMCR;
        g_dbg.sai4_cr1     = SAI4_Block_A->CR1;   /* bit 16 = SAIEN — must be 1 during recording */
        g_dbg.dma_cr       = DMA1_Stream1->CR;
        g_dbg.dma_ndtr     = DMA1_Stream1->NDTR;
        g_dbg.dma_m0ar     = DMA1_Stream1->M0AR;
        g_dbg.sitp         = g_dbg.ch0_cfg1 & 0x3u;
        g_dbg.spicksel     = (g_dbg.ch0_cfg1 >> 2u) & 0x3u;
        g_dbg.dtrbs        = (g_dbg.ch0_cfg2 >> 3u) & 0x1Fu;
        /* FLTRDATAR bits[31:8] = 24-bit signed result (left-aligned).
         * Cast to int32_t BEFORE shift → arithmetic shift → sign bit preserved. */
        g_dbg.last_raw     = (uint32_t)DFSDM1_Filter0->FLTRDATAR;
        g_dbg.last_val     = (int32_t)((int32_t)g_dbg.last_raw >> 8);
        g_dbg.uptime_ticks++;

        /* CKABF[0] = bit 16 of FLTISR — sticky flag: set at boot before SAI4 started,
         * never cleared by software (needs FLTICR write). Report once only to avoid
         * flooding the log. The flag being permanently set is expected behavior;
         * it does NOT mean clock is absent during recording (DMA callbacks prove clock). */
        static uint8_t s_ckabf_reported = 0u;
        if (!s_ckabf_reported && (g_dbg.flt0_isr & (1u << 16)))
        {
            SEGGER_RTT_WriteString(0, "[WARN] DFSDM CKABF0 set (sticky boot flag — SAI4 was absent at init, normal)\r\n");
            s_ckabf_reported = 1u;
        }

        /* ── Drain all pending log entries → RTT + Terminal I/O ─────────────
         * printf is safe at priority 1: any real-time task preempts the spin. */
        LogMsg_t e;
        while (xQueueReceive(xLogQueue, &e, 0) == pdTRUE)
        {
            char buf[96];
            int  n = snprintf(buf, sizeof(buf), "[T+%7lu ms] %s %lu\r\n",
                              e.timestamp, e.pcMsg, e.val);
            emit(buf, n);
        }

        /* ── Every 1 second: drop counter + 5-second health ping ─────────── */
        if (++cycleCount >= 5u)
        {
            cycleCount = 0u;

            /* Drop counter — report and reset if non-zero */
            uint32_t dropped = g_logDropped;
            if (dropped > 0u)
            {
                g_logDropped = 0u;
                char dbuf[64];
                int  dn = snprintf(dbuf, sizeof(dbuf), "[T+%7lu ms] LOG_DROPPED %lu\r\n",
                                   HAL_GetTick(), dropped);
                emit(dbuf, dn);
            }

            /* ── Periodic system health ping — every 5 seconds ──────────────
             * Appears in both RTT and Terminal I/O even when nothing is
             * recording, so both viewers confirm the system is alive.         */
            if (++pingCount >= 5u)
            {
                pingCount = 0u;
                static const char * const stateStr[] = { "IDLE", "RECORDING", "SAVING" };
                RecState_t st = VoiceRec_GetState();
                const char *stName = (st <= REC_SAVING) ? stateStr[st] : "?";

                char pbuf[192];
                int  pn;

                pn = snprintf(pbuf, sizeof(pbuf),
                    "[T+%7lu ms] [PING] state=%s  sd_free_KB=%lu  dfsdm_ovr=%lu  sd_miss=%lu\r\n",
                    HAL_GetTick(), stName,
                    (unsigned long)g_sdFreeKB,
                    (unsigned long)g_AudioHealth.dfsdm_overruns,
                    (unsigned long)g_AudioHealth.buffer_misses);
                emit(pbuf, pn);

                pn = snprintf(pbuf, sizeof(pbuf),
                    "[T+%7lu ms] [PING] VoiceRecTask HWM=%u/3072 words  sd_worst_ms=%lu\r\n",
                    HAL_GetTick(),
                    (unsigned)uxTaskGetStackHighWaterMark(voiceRecTaskHandle),
                    (unsigned long)g_AudioHealth.sd_write_max_ms);
                emit(pbuf, pn);

                pn = snprintf(pbuf, sizeof(pbuf),
                    "[T+%7lu ms] [PING] uart_tx=%lu  tx_retry=%lu  tx_fail=%lu  rx_ore=%lu  last_err=0x%02lX  last=%lu/%lu B\r\n",
                    HAL_GetTick(),
                    (unsigned long)g_UartHealth.tx_transfer_count,
                    (unsigned long)g_UartHealth.tx_retries,
                    (unsigned long)g_UartHealth.tx_hard_fails,
                    (unsigned long)g_UartHealth.rx_overruns,
                    (unsigned long)g_UartHealth.tx_last_err_code,
                    (unsigned long)g_UartHealth.tx_last_sent,
                    (unsigned long)g_UartHealth.tx_last_declared);
                emit(pbuf, pn);

                /* ── DFSDM + SAI4 register health — read from g_dbg ───────────
                 * g_dbg at 0x24000050 is refreshed every 200 ms at the top of
                 * this loop, so values here are always current.
                 *
                 * All registers are now in g_dbg — refreshed every 200 ms above.
                 * Read from struct for a single consistent snapshot per ping. */
                {
                    /* Snapshot all fields from g_dbg */
                    uint32_t ch0cfg1   = g_dbg.ch0_cfg1;
                    uint32_t ch0cfg2   = g_dbg.ch0_cfg2;
                    uint32_t fltcr1    = g_dbg.flt0_cr1;
                    uint32_t fltcr2    = g_dbg.flt0_cr2;
                    uint32_t fltfcr    = g_dbg.flt0_fcr;
                    uint32_t fltisr    = g_dbg.flt0_isr;
                    uint32_t sai4pdmcr = g_dbg.sai4_pdm;
                    uint32_t dma_cr    = g_dbg.dma_cr;
                    uint32_t ndtr      = g_dbg.dma_ndtr;
                    uint32_t m0ar      = g_dbg.dma_m0ar;
                    uint32_t sitp_val  = g_dbg.sitp;
                    uint32_t spick_val = g_dbg.spicksel;
                    uint32_t dtrbs_val = g_dbg.dtrbs;
                    uint32_t last_raw  = g_dbg.last_raw;
                    int32_t  last_val  = g_dbg.last_val;

                    /* Decode FLTISR status tag */
                    uint8_t ckabf = (uint8_t)((fltisr >> 16) & 0xFFu);
                    uint8_t ovr   = (uint8_t)((fltisr >>  3) & 0x01u);
                    const char *isrTag;
                    if      (ckabf && ovr) isrTag = "CKABF+OVR!";
                    else if (ckabf)        isrTag = "CKABF!";
                    else if (ovr)          isrTag = "OVR!";
                    else                   isrTag = "OK";

                    /* ── CLOCK CHECK — only what matters for PE2 reaching the mic ── */
                    uint32_t sai4cr1   = g_dbg.sai4_cr1;
                    uint32_t saien_bit = (sai4cr1 >> 16u) & 0x1u;   /* bit 16 = SAIEN */

                    /* Line 1 — SAI4 clock status: is PE2 running? */
                    pn = snprintf(pbuf, sizeof(pbuf),
                        "[T+%7lu ms] [CLK] SAI4PDMCR=%08lX[%s] SAI4CR1=%08lX SAIEN=%lu[%s] CKABF=%02X[%s]\r\n",
                        (unsigned long)HAL_GetTick(),
                        (unsigned long)sai4pdmcr, (sai4pdmcr == 0x00000101u) ? "OK" : "FAIL",
                        (unsigned long)sai4cr1,
                        (unsigned long)saien_bit, (saien_bit == 1u)           ? "OK" : "FAIL-PE2_FLAT",
                        (unsigned)ckabf,          (ckabf     == 0u)           ? "OK" : "NO_CLK");
                    emit(pbuf, pn);

                    /* Line 2 — BDMA Channel1: draining SAI4 RX FIFO keeps PE2 alive.
                     * CCR bit0=EN=1 means DMA active. CNDTR must be non-zero and
                     * changing between pings — if it freezes, BDMA stalled. */
                    {
                        uint32_t bdma_ccr   = BDMA_Channel1->CCR;
                        uint32_t bdma_cndtr = BDMA_Channel1->CNDTR;
                        pn = snprintf(pbuf, sizeof(pbuf),
                            "[T+%7lu ms] [BDMA] CCR=%08lX[%s] CNDTR=%lu\r\n",
                            (unsigned long)HAL_GetTick(),
                            (unsigned long)bdma_ccr,
                            (bdma_ccr & 0x1u) ? "EN-OK" : "OFF-PE2_DEAD",
                            (unsigned long)bdma_cndtr);
                        emit(pbuf, pn);
                    }

                    /* Lines 3 & 4 commented out — not relevant to clock check.
                     * Re-enable after PE2 is confirmed alive on scope/ITM.
                    pn = snprintf(pbuf, sizeof(pbuf),
                        "[DFSDM] Ch1CFG1=%08lX SITP=%lu SPICKSEL=%lu Ch1CFG2=%08lX DTRBS=%lu\r\n", ...);
                    pn = snprintf(pbuf, sizeof(pbuf),
                        "[DFSDM] FLTCR1=%08lX FLTCR2=%08lX FLTFCR=%08lX\r\n", ...);
                    pn = snprintf(pbuf, sizeof(pbuf),
                        "[DFSDM] DMA_CR=%08lX NDTR=%lu M0AR=%08lX last_raw=%08lX last_val=%ld\r\n", ...);
                    */
                }
            }
        }

        /* ── SD_DETECT (PI8, active-low) every 200 ms — LED2 (PI13) mirrors ── */
        GPIO_PinState det = HAL_GPIO_ReadPin(SD_DETECT_PORT, SD_DETECT_PIN);
        HAL_GPIO_WritePin(LED2_PORT, LED2_PIN,
                          (det == GPIO_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET);

        vTaskDelay(pdMS_TO_TICKS(200u));
    }
}
