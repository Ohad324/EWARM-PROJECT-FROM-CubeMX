#include "music_display_task.h"
#include "jpeg_decoder.h"
#include "log_mutex.h"
#include "timing_log.h"    /* TLOG(), T_US() */
#include "itm_log.h"       /* STAGE() — ITM PORT[0] + RTT WriteString, zero printf */
#include "main.h"          /* huart8 */
#include "stm32h7xx.h"     /* LTDC, DMA1, DMA2, SCB peripheral handles */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include <string.h>
#include <stdio.h>

#ifndef RELEASE_BUILD
/* Bug Y / FMC-conflict probe per Gemini hint — sticky-flag register snapshot
 * called at each TGFX stage. Catches the exact moment a flag flips (LTDC FIFO
 * underrun, DMA transfer error, CPU bus fault). Bits to look for:
 *   LTDC.ISR  bit 1 (FUIF)    SDRAM starvation → Gemini's theory
 *   LTDC.ISR  bit 2 (TERRIF)  LTDC transfer error
 *   DMA*.LISR/HISR TEIF bits  DMA transfer errors
 *   SCB.CFSR  bit 8 IBUSERR / bit 9 PRECISERR  CPU bus fault
 *   SCB.BFAR  faulting address (in 0xD0000000 range = SDRAM bank 2)         */
#define LCD_STAGE_DUMP(label)                                                     \
    do {                                                                          \
        LOG("[%s] LTDC.ISR=0x%08lX D1L=0x%08lX D1H=0x%08lX D2L=0x%08lX "          \
            "D2H=0x%08lX CFSR=0x%08lX BFAR=0x%08lX\n",                            \
            (label),                                                              \
            (unsigned long)LTDC->ISR,                                             \
            (unsigned long)DMA1->LISR, (unsigned long)DMA1->HISR,                 \
            (unsigned long)DMA2->LISR, (unsigned long)DMA2->HISR,                 \
            (unsigned long)SCB->CFSR, (unsigned long)SCB->BFAR);                  \
    } while (0)
#else
#define LCD_STAGE_DUMP(label) ((void)0)
#endif

/* DWT timestamps set by UARTReceiveTask (main.c) */
extern volatile uint32_t g_t_track;
extern volatile uint32_t g_t_thumb;
#define DWT_CYCLES_PER_MS  480000UL
#define DWT_MS(c)          ((c) / DWT_CYCLES_PER_MS)
#define DWT_SNAP()         (DWT->CYCCNT)

extern UART_HandleTypeDef huart8;

/* ── Queue handles ─────────────────────────────────────────────────────── */
QueueHandle_t xMusicQueue;
QueueHandle_t xMusicDoneQueue;

static bool s_jpegInitDone = false;

/* ── Public API ─────────────────────────────────────────────────────────── */

void Music_Init(void)
{
    xMusicQueue     = xQueueCreate(4u, sizeof(music_msg_t));
    xMusicDoneQueue = xQueueCreate(4u, sizeof(music_done_msg_t));
    configASSERT(xMusicQueue     != NULL);
    configASSERT(xMusicDoneQueue != NULL);
    /* JpegDisplayTask removed — JPEG decode now runs inside TouchGFXTask
     * via Music_Poll() called from Model::tick(). */
    LOG("[MUSIC] Music_Init() -- queues created\n");
}

/* ── Music_Poll — non-blocking, called every TouchGFX frame ─────────────
 * Drains one entry from xMusicQueue per call (timeout=0).
 * If the entry is MSG_THUMB, JPEG_Decode() blocks ~200–500 ms — the
 * TouchGFX frame loop is suspended for that duration (accepted trade-off
 * for merging all display work into one task at priority 16).           */
void Music_Poll(void)
{
    if (!s_jpegInitDone) { JPEG_Init(); s_jpegInitDone = true; }

    music_msg_t raw;
    if (xQueueReceive(xMusicQueue, &raw, 0) != pdTRUE)
        return;

    music_done_msg_t done;
    memset(&done, 0, sizeof(done));

    switch (raw.type)
    {
    /* ── MSG_TRACK ──────────────────────────────────────────────── */
    case MSG_TRACK:
        STAGE("TGFX1 PASS");
        LCD_STAGE_DUMP("HW@TGFX1");
        done.type = MUSIC_DONE_TRACK;
        strncpy(done.track.title,   raw.track.title,
                sizeof(done.track.title)   - 1u);
        strncpy(done.track.artist,  raw.track.artist,
                sizeof(done.track.artist)  - 1u);
        strncpy(done.track.videoId, raw.track.videoId,
                sizeof(done.track.videoId) - 1u);

        if (xQueueSend(xMusicDoneQueue, &done, pdMS_TO_TICKS(10)) != pdTRUE)
        {
            STAGE("TGFX2 FAIL");
            LOG("[WARN] xMusicDoneQueue full -- TRACK dropped\n");
        }
        else
        {
            STAGE("TGFX2 PASS");
            LCD_STAGE_DUMP("HW@TGFX2");
            LOG("[4] Track sent to screen: \"%s\" by \"%s\""
                "  [%lu ms from track received]\n",
                raw.track.title, raw.track.artist,
                DWT_MS(DWT_SNAP() - g_t_track));
        }
        break;

    /* ── MSG_THUMB ──────────────────────────────────────────────── */
    case MSG_THUMB:
    {
        uint32_t w = 0u, h = 0u;
        STAGE("TGFX3 PASS");
        LCD_STAGE_DUMP("HW@TGFX3");
        uint32_t t_dec_start = DWT_SNAP();
        HAL_StatusTypeDef st =
            JPEG_Decode(raw.thumb.data, raw.thumb.size, &w, &h);
        uint32_t dec_cycles = DWT_SNAP() - t_dec_start;

        if (st == HAL_OK)
        {
            STAGE("TGFX4 PASS");
            LCD_STAGE_DUMP("HW@TGFX4");
            done.type         = MUSIC_DONE_THUMB;
            done.thumb.rgb888 = JPEG_GetRGB888Buffer();
            done.thumb.width  = w;
            done.thumb.height = h;

            if (xQueueSend(xMusicDoneQueue, &done, pdMS_TO_TICKS(10)) != pdTRUE)
            {
                STAGE("TGFX5 FAIL");
                LOG("[WARN] xMusicDoneQueue full -- THUMB dropped\n");
            }
            else
            {
                STAGE("TGFX5 PASS");
                LCD_STAGE_DUMP("HW@TGFX5");
                LOG("[4] Thumbnail sent to screen: %lu x %lu px"
                    "  [decode: %lu ms  |  total: %lu ms]\n",
                    w, h, DWT_MS(dec_cycles),
                    DWT_MS(DWT_SNAP() - g_t_track));
            }
        }
        else
        {
            STAGE("TGFX4 FAIL");
            LOG("[ERROR] JPEG decode FAILED  [%lu ms]\n", DWT_MS(dec_cycles));
        }
        break;
    }

    /* ── MSG_ERROR ──────────────────────────────────────────────── */
    case MSG_ERROR:
        STAGE("TGFX1 FAIL");
        LOG("[ERROR] Display error: %s\n", raw.error.reason);
        done.type = MUSIC_DONE_ERROR;
        strncpy(done.error.reason, raw.error.reason,
                sizeof(done.error.reason) - 1u);
        if (xQueueSend(xMusicDoneQueue, &done, pdMS_TO_TICKS(10)) != pdTRUE)
            LOG("[MUSIC] WARNING: xMusicDoneQueue full -- ERROR dropped\n");
        break;

    default:
        break;
    }
}

/**
 * Phase 2 stub: send CTRL command to NORA.
 * STM32 TX = PD0 (huart8). NORA RX = GPIO18.
 * Uncomment the HAL_UART_Transmit line in Phase 2.
 */
void Music_SendCtrl(const char *action)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "CTRL:%s\n", action);
    /* Phase 2: HAL_UART_Transmit(&huart8, (uint8_t *)buf, strlen(buf), 100); */
    LOG("[MUSIC] Phase2 stub: CTRL:%s (not sent)\n", action);
}

/**
 * Phase 2 stub: progress bar timer callback.
 * Register with xTimerCreate in Phase 2.
 */
void Music_ProgressTimerCallback(TimerHandle_t xTimer)
{
    (void)xTimer;
    /* Phase 2: increment elapsed seconds, update progressFill width */
}

/* ── JpegDisplayTask — kept for reference, never compiled ───────────────
 * Replaced by Music_Poll() called from Model::tick() inside TouchGFXTask. */
#if 0
static void JpegDisplayTask(void *argument)
{
    (void)argument;
    LOG("[MUSIC] JpegDisplayTask started\n");

    JPEG_Init();    /* initialise colour-conversion lookup tables once */

    music_msg_t      raw;
    music_done_msg_t done;

    for (;;)
    {
        if (xQueueReceive(xMusicQueue, &raw, portMAX_DELAY) != pdTRUE)
            continue;

        memset(&done, 0, sizeof(done));

        switch (raw.type)
        {
        /* ── MSG_TRACK ──────────────────────────────────────────────── */
        case MSG_TRACK:
            done.type = MUSIC_DONE_TRACK;
            strncpy(done.track.title,   raw.track.title,
                    sizeof(done.track.title)   - 1u);
            strncpy(done.track.artist,  raw.track.artist,
                    sizeof(done.track.artist)  - 1u);
            strncpy(done.track.videoId, raw.track.videoId,
                    sizeof(done.track.videoId) - 1u);

            if (xQueueSend(xMusicDoneQueue, &done, pdMS_TO_TICKS(10)) != pdTRUE)
                LOG("[WARN] xMusicDoneQueue full -- TRACK dropped\n");
            else
                LOG("[4] Track sent to screen: \"%s\" by \"%s\""
                    "  [%lu ms / %lu Kcycles from track received]\n",
                    raw.track.title, raw.track.artist,
                    DWT_MS(DWT_SNAP() - g_t_track),
                    (DWT_SNAP() - g_t_track) / 1000UL);
            break;

        /* ── MSG_THUMB ──────────────────────────────────────────────── */
        case MSG_THUMB:
        {
            uint32_t w = 0u, h = 0u;
            uint32_t t6_us = T_US();
            TLOG("6 JPEG_Decode_start  t=%lu us", t6_us);
            uint32_t t_dec_start = DWT_SNAP();
            HAL_StatusTypeDef st =
                JPEG_Decode(raw.thumb.data, raw.thumb.size, &w, &h);
            uint32_t t_dec_end = DWT_SNAP();
            uint32_t dec_cycles = t_dec_end - t_dec_start;
            uint32_t t7_us = T_US();
            TLOG("7 JPEG_Decode_done %lux%lu  st=%d  t=%lu us  dur=%lu us",
                 w, h, (int)st, t7_us, t7_us - t6_us);

            if (st == HAL_OK)
            {
                done.type         = MUSIC_DONE_THUMB;
                done.thumb.rgb888 = JPEG_GetRGB888Buffer(); /* 800x480 scaled */
                done.thumb.width  = w;
                done.thumb.height = h;

                if (xQueueSend(xMusicDoneQueue, &done, pdMS_TO_TICKS(10)) != pdTRUE)
                    LOG("[WARN] xMusicDoneQueue full -- THUMB dropped\n");
                else
                    LOG("[4] Thumbnail sent to screen: %lu x %lu px"
                        "  [decode: %lu ms / %lu Kcycles"
                        "  |  total: %lu ms]\n",
                        w, h,
                        DWT_MS(dec_cycles), dec_cycles / 1000UL,
                        DWT_MS(t_dec_end - g_t_track));
            }
            else
            {
                LOG("[ERROR] JPEG decode FAILED"
                    "  [decode attempt: %lu ms / %lu Kcycles]\n",
                    DWT_MS(dec_cycles), dec_cycles / 1000UL);
            }
            break;
        }

        /* ── MSG_ERROR ──────────────────────────────────────────────── */
        case MSG_ERROR:
            LOG("[ERROR] Display error: %s\n", raw.error.reason);

            done.type = MUSIC_DONE_ERROR;
            strncpy(done.error.reason, raw.error.reason,
                    sizeof(done.error.reason) - 1u);

            if (xQueueSend(xMusicDoneQueue, &done, pdMS_TO_TICKS(10)) != pdTRUE)
                LOG("[MUSIC] WARNING: xMusicDoneQueue full -- ERROR dropped\n");
            break;

        default:
            break;
        }
    }
}
#endif /* 0 — JpegDisplayTask */
