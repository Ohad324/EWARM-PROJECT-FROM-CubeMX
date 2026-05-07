#include "music_display_task.h"
#include "jpeg_decoder.h"
#include "log_mutex.h"
#include "timing_log.h"    /* TLOG(), T_US() */
#include "main.h"          /* huart8 */
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include <string.h>
#include <stdio.h>

#ifndef RELEASE_BUILD
#include "test_thumb_jpeg.h"   /* kTestThumbJpeg[] -- offline PNG-as-thumbnail fixture */
#include "stm32h7xx.h"         /* SCB_CleanDCache_by_Addr */

/* SDRAM-resident landing pad for the test JPEG. JPEG_Decode() requires its
 * input in SDRAM (DMA/MDMA accessible). Sized 16 KB -- fits the 10 KB fixture
 * with margin, far cheaper than placing kTestThumbJpeg itself in SDRAM. */
static uint8_t s_testThumbSdram[16384u]
    __attribute__((section(".sdram_bss"), aligned(32)));
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

/* Priority matches osPriorityBelowNormal (16) — same value CMSIS passed to FreeRTOS */
#define MUSIC_TASK_PRIORITY  16u
#define MUSIC_TASK_STACK_WORDS  (16384u / sizeof(StackType_t))   /* 16 KB */

static void JpegDisplayTask(void *argument);

/* ── Public API ─────────────────────────────────────────────────────────── */

void Music_Init(void)
{
    xMusicQueue     = xQueueCreate(4u, sizeof(music_msg_t));
    xMusicDoneQueue = xQueueCreate(4u, sizeof(music_done_msg_t));
    configASSERT(xMusicQueue     != NULL);
    configASSERT(xMusicDoneQueue != NULL);

    xTaskCreate(JpegDisplayTask, "music_disp",
                MUSIC_TASK_STACK_WORDS, NULL, MUSIC_TASK_PRIORITY, NULL);
    LOG("[MUSIC] Music_Init() -- queues + task created\n");
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

/* ── Test-only injection ───────────────────────────────────────────────── */

#ifndef RELEASE_BUILD
/**
 * Synthesize a MSG_THUMB carrying the embedded test JPEG and push it to
 * xMusicQueue, exactly as if it had arrived from NORA over UART. Lets us
 * exercise the full thumbnail pipeline (HW JPEG decode -> scale 800x480
 * -> DMA2D -> LTDC) with no WiFi connection.
 */
static void Music_InjectTestThumb(void)
{
    memcpy(s_testThumbSdram, kTestThumbJpeg, TEST_THUMB_JPEG_SIZE);
    SCB_CleanDCache_by_Addr((uint32_t *)s_testThumbSdram,
                             (int32_t)((TEST_THUMB_JPEG_SIZE + 31u) & ~31u));

    music_msg_t raw;
    raw.type        = MSG_THUMB;
    raw.thumb.data  = s_testThumbSdram;
    raw.thumb.size  = TEST_THUMB_JPEG_SIZE;

    if (xQueueSend(xMusicQueue, &raw, pdMS_TO_TICKS(100)) != pdTRUE)
        LOG("[TEST-INJECT] xMusicQueue full -- test thumb dropped\n");
    else
        LOG("[TEST-INJECT] Test thumb pushed to xMusicQueue (%u bytes)\n",
            (unsigned)TEST_THUMB_JPEG_SIZE);
}

/* ISR-safe trigger: post MSG_THUMB pointing at flash-resident kTestThumbJpeg.
 * No memcpy/cache flush needed -- flash is read-only and JPEG MDMA bypasses
 * CPU caches. JpegDisplayTask receives, decodes, scales, blits. */
BaseType_t Music_RequestTestThumbFromISR(BaseType_t *pxHigherPriorityTaskWoken)
{
    if (xMusicQueue == NULL) return pdFALSE;

    music_msg_t m;
    m.type       = MSG_THUMB;
    m.thumb.data = (uint8_t *)kTestThumbJpeg;   /* flash, MDMA-readable */
    m.thumb.size = TEST_THUMB_JPEG_SIZE;
    return xQueueSendFromISR(xMusicQueue, &m, pxHigherPriorityTaskWoken);
}
#endif

/* ── Task body ─────────────────────────────────────────────────────────── */

static void JpegDisplayTask(void *argument)
{
    (void)argument;
    LOG("[MUSIC] JpegDisplayTask started\n");

    JPEG_Init();    /* initialise colour-conversion lookup tables once */

    /* Test thumb injection is now triggered by PC13 blue button press
     * (HAL_GPIO_EXTI_Callback in voice_recorder.c -> Music_RequestTestThumbFromISR).
     * No auto T+3s injection. */

    music_msg_t      raw;
    music_done_msg_t done;

    for (;;)
    {
        /* Block on real music messages only -- no periodic re-inject (one
         * injection above is enough to verify the full pipeline visually) */
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
