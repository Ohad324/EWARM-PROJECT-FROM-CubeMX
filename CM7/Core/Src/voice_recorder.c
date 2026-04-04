/*
 * voice_recorder.c — 5-second button-triggered voice recorder
 *
 * PIPELINE:
 *   Blue button PC13 → ISR → xTaskNotifyFromISR(voiceRecTaskHandle)
 *   VoiceRecTask wakes → HAL_DFSDM_FilterRegularStart_DMA
 *   DMA half/full callbacks → give s_dmaSem (counting, depth 4)
 *   VoiceRecTask drain loop → StoreDmaChunk() → int32>>8→int16 → g_AudioBuf (SDRAM)
 *   After 5 s → stop DMA → xQueueSend(xVoiceQueue) → SDWriteTask wakes
 *   SDWriteTask → f_open/f_write(WAV header + PCM)/f_close → xQueueSend(xLogQueue)
 *   RTTLogTask → SEGGER_RTT_printf result
 *
 * HARDWARE:
 *   DFSDM1 Channel 0 / Filter 0  (owns these — audio_rec.c is disabled)
 *   DMA1 Stream1                  (DMAMUX request: DFSDM1_FLT0)
 *   Button: PC13 EXTI15_10, falling edge
 *   LED:    PI12 (LED1 on STM32H747I-DISCO, configured by MX_GPIO_Init)
 *
 * DFSDM clock math (16 kHz):
 *   APB2 = 100 MHz
 *   CKOUTDIV = 24  →  CKOUT = 100 MHz / (2 × 25) = 2 MHz
 *   OSR = 125       →  PCM rate = 2,000,000 / 125 = 16,000 Hz  ✓
 *   RightBitShift = 8  →  16-bit audio in bits [23:8] of 32-bit DFSDM result
 */

#include "voice_recorder.h"
#include "main.h"           /* LED1_Pin, LED1_GPIO_Port, Error_Handler */
#include "stm32h7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "ff.h"             /* FatFS f_open/f_write/f_close */
#include "audio_sd.h"       /* AudioSD_GetErrorCode() — HealthMonTask */
#include "SEGGER_RTT.h"         /* RTTLogTask uses SEGGER_RTT_Write */
#include <string.h>
#include <stdio.h>       /* snprintf */
#include <limits.h>      /* ULONG_MAX */

/* ── Audio constants ─────────────────────────────────────────────────────── */
#define SAMPLE_RATE           16000u
#define RECORD_DURATION_S     5u
#define AUDIO_BUFFER_SAMPLES  (SAMPLE_RATE * RECORD_DURATION_S)  /* 80000 */
#define DMA_HALF_SIZE         256u                 /* samples per DMA half  */
#define DMA_BUF_TOTAL         (DMA_HALF_SIZE * 2u) /* 512 int32_t total     */
#define RECORD_MS             (RECORD_DURATION_S * 1000u)
#define DEBOUNCE_MS           300u

/* ── WAV header (44 bytes, little-endian, packed) ───────────────────────── */
typedef struct {
    char     riff[4];        /* "RIFF"                           */
    uint32_t chunkSize;      /* total file size minus 8 bytes    */
    char     wave[4];        /* "WAVE"                           */
    char     fmt[4];         /* "fmt "                           */
    uint32_t subchunk1Size;  /* 16 for PCM                       */
    uint16_t audioFormat;    /* 1 = PCM (uncompressed)           */
    uint16_t numChannels;    /* 1 = mono                         */
    uint32_t sampleRate;     /* 16000                            */
    uint32_t byteRate;       /* sampleRate × channels × bps/8    */
    uint16_t blockAlign;     /* channels × bps/8 = 2             */
    uint16_t bitsPerSample;  /* 16                               */
    char     data[4];        /* "data"                           */
    uint32_t subchunk2Size;  /* PCM data byte count              */
} WavHdr_t;

/* ── Peripheral handles (private to this file) ──────────────────────────── */
static DFSDM_Channel_HandleTypeDef s_hdfsdm_ch;
static DFSDM_Filter_HandleTypeDef  s_hdfsdm_flt;
static DMA_HandleTypeDef           s_hdma;

/* ── Audio buffers ───────────────────────────────────────────────────────── */

/* DMA ping-pong buffer — in AXI SRAM (default .bss).
 * DMA1 can access AXI SRAM (0x24xxxxxx) via the D2→D1 bus matrix path.
 * 32-byte aligned for SCB_InvalidateDCache_by_Addr(). */
static int32_t g_DmaBuf[DMA_BUF_TOTAL] __attribute__((aligned(32)));

/* Audio accumulation buffer — 5 s × 16000 Hz × 2 bytes = 160 KB in SDRAM.
 * Placed in .sdram_bss (already in stm32h747xx_flash_CM7.icf → SDRAM_region).
 * NOT initialized by the startup copy loop — that's correct for audio scratch. */
int16_t g_AudioBuf[AUDIO_BUFFER_SAMPLES]
    __attribute__((section(".sdram_bss"), aligned(32)));

/* ── Recording state ─────────────────────────────────────────────────────── */
volatile RecState_t  g_State       = REC_IDLE;
volatile uint32_t    g_SampleCount = 0u;
static   uint32_t    g_FileIndex   = 0u;
volatile SysMode_t   g_SysMode     = SYS_MODE_RECORD;

/* ── FreeRTOS objects ────────────────────────────────────────────────────── */
TaskHandle_t       voiceRecTaskHandle = NULL;
QueueHandle_t      xLogQueue          = NULL;

/* Counting semaphore — DMA callbacks give it, VoiceRecTask drain loop takes it.
 * Depth 4: handles bursts where both half+full callbacks fire before task runs. */
static SemaphoreHandle_t s_dmaSem = NULL;

/* ── LED helpers ─────────────────────────────────────────────────────────── */
#define LED_ON()     HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET)
#define LED_OFF()    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET)
#define LED_TOGGLE() HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)

/* ── Forward declarations ────────────────────────────────────────────────── */
static void Button_GPIO_Init(void);
static void DFSDM_HW_Init(void);
static void DMA_HW_Init(void);
static void StoreDmaChunk(void);
static void BuildWavHdr(WavHdr_t *h, uint32_t nSamples);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

RecState_t VoiceRec_GetState(void)
{
    return g_State;
}

/* Lightweight init — button GPIO + NVIC only.
 * Safe to call without touching DFSDM or DMA.
 * Use this during bring-up to verify button + RTT before enabling recording. */
void VoiceRec_ButtonInit(void)
{
    g_State   = REC_IDLE;
    g_SysMode = SYS_MODE_RECORD;
    __DSB();
    Button_GPIO_Init();
}

void VoiceRec_Init(void)
{
    /* Create FreeRTOS objects before enabling any IRQs */
    s_dmaSem  = xSemaphoreCreateCounting(4u, 0u);
    xLogQueue = xQueueCreate(4u, sizeof(LogMsg_t));
    configASSERT(s_dmaSem);
    configASSERT(xLogQueue);

    Button_GPIO_Init();
    DFSDM_HW_Init();
    DMA_HW_Init();

    g_State       = REC_IDLE;
    g_SampleCount = 0u;
    g_FileIndex   = 0u;
    g_SysMode     = SYS_MODE_RECORD;
    __DSB();
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HAL callbacks (override weak HAL defaults)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Blue button PC13 — EXTI falling edge */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != GPIO_PIN_13) return;

    /* Simple debounce — ignore presses within DEBOUNCE_MS of the last one */
    static uint32_t s_lastPress = 0u;
    uint32_t now = HAL_GetTick();
    if ((now - s_lastPress) < DEBOUNCE_MS) return;
    s_lastPress = now;

    HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin);  /* visual confirmation */

    /* Only notify task if it has been created */
    if (g_SysMode == SYS_MODE_RECORD && g_State == REC_IDLE
        && voiceRecTaskHandle != NULL)
    {
        BaseType_t higher = pdFALSE;
        xTaskNotifyFromISR(voiceRecTaskHandle, 1u, eSetBits, &higher);
        portYIELD_FROM_ISR(higher);
    }
}

/* DMA half-complete — first DMA_HALF_SIZE samples (indices 0..DMA_HALF_SIZE-1) ready */
void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;

    /* Invalidate D-Cache for first half so CPU reads fresh DMA data */
    SCB_InvalidateDCache_by_Addr((uint32_t *)&g_DmaBuf[0],
                                  DMA_HALF_SIZE * sizeof(int32_t));

    BaseType_t higher = pdFALSE;
    xSemaphoreGiveFromISR(s_dmaSem, &higher);
    portYIELD_FROM_ISR(higher);
}

/* DMA full-complete — second DMA_HALF_SIZE samples (indices DMA_HALF_SIZE..511) ready */
void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;

    /* Invalidate D-Cache for second half */
    SCB_InvalidateDCache_by_Addr((uint32_t *)&g_DmaBuf[DMA_HALF_SIZE],
                                  DMA_HALF_SIZE * sizeof(int32_t));

    BaseType_t higher = pdFALSE;
    xSemaphoreGiveFromISR(s_dmaSem, &higher);
    portYIELD_FROM_ISR(higher);
}

/* IRQ trampoline — called from DMA1_Stream1_IRQHandler in stm32h7xx_it.c */
void VoiceRec_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  VoiceRecTask — wakes on trigger, active-drains DMA for 5 s, signals SD
 * ═══════════════════════════════════════════════════════════════════════════ */
void VoiceRecTask(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    uint32_t notif;

    for (;;)
    {
        /* Block until button ISR (Phase 1) or WakeWordTask (Phase 2) notifies */
        xTaskNotifyWait(0u, ULONG_MAX, &notif, portMAX_DELAY);

        /* Ignore spurious notifications if not idle */
        if (g_State != REC_IDLE) continue;

        g_SampleCount = 0u;
        g_State       = REC_RECORDING;
        __DSB();

        /* Start DFSDM DMA */
        HAL_StatusTypeDef hal =
            HAL_DFSDM_FilterRegularStart_DMA(&s_hdfsdm_flt,
                                              g_DmaBuf,
                                              DMA_BUF_TOTAL);
        if (hal != HAL_OK)
        {
            /* Post DMA_START error directly to log queue */
            LogMsg_t err = {0};
            err.result    = REC_ERR_DMA_START;
            err.timestamp = HAL_GetTick();
            strncpy(err.filename, "NONE", sizeof(err.filename));
            xQueueSend(xLogQueue, &err, pdMS_TO_TICKS(100));
            g_State = REC_IDLE; __DSB();
            continue;
        }

        /* ── Active drain loop ──────────────────────────────────────────── */
        /* NEVER use vTaskDelay here — DMA is running and must be drained.  */
        TickType_t deadline   = xTaskGetTickCount() + pdMS_TO_TICKS(RECORD_MS);
        TickType_t ledToggle  = xTaskGetTickCount() + pdMS_TO_TICKS(1000u);

        while (xTaskGetTickCount() < deadline)
        {
            /* Toggle LED every 1 s during recording */
            if (xTaskGetTickCount() >= ledToggle)
            {
                LED_TOGGLE();
                ledToggle += pdMS_TO_TICKS(1000u);  /* fixed interval, no drift */
            }

            /* Wait up to 10 ms for a DMA half/full event */
            if (xSemaphoreTake(s_dmaSem, pdMS_TO_TICKS(10u)) == pdTRUE)
            {
                StoreDmaChunk();
            }

            /* Safety cap — stop early if SDRAM buffer is full */
            if (g_SampleCount >= AUDIO_BUFFER_SAMPLES)
            {
                break;
            }
        }

        /* Stop DMA */
        HAL_DFSDM_FilterRegularStop_DMA(&s_hdfsdm_flt);

        /* Drain any stale semaphore tokens left by callbacks that arrived
         * during or just after the stop */
        while (xSemaphoreTake(s_dmaSem, 0) == pdTRUE) {}

        LED_ON();   /* stay ON — recording done, saving to SD */

        /* Signal SDWriteTask */
        uint32_t msg = 1u;
        xQueueSend(q, &msg, 0);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  SDWriteTask — wakes on queue, writes WAV to SD card
 * ═══════════════════════════════════════════════════════════════════════════ */
void SDWriteTask(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    uint32_t msg;

    for (;;)
    {
        /* If USB-MSC owns the SD card, sleep and check again */
        if (g_SysMode != SYS_MODE_RECORD)
        {
            vTaskDelay(pdMS_TO_TICKS(100u));
            continue;
        }

        xQueueReceive(q, &msg, portMAX_DELAY);

        /* Re-check mode after waking — mode may have changed while blocked */
        if (g_SysMode != SYS_MODE_RECORD)
        {
            g_State = REC_IDLE; __DSB();
            continue;
        }

        /* ── Mark saving state BEFORE any SD operations ─────────────────── */
        g_State = REC_SAVING;
        __DSB();

        /* Freeze sample count immediately — VoiceRecTask must not change it */
        uint32_t samplesSnapshot = g_SampleCount;

        LogMsg_t logMsg = {0};
        logMsg.timestamp = HAL_GetTick();
        logMsg.sizeBytes  = samplesSnapshot * sizeof(int16_t);
        logMsg.result     = REC_OK;

        /* Build filename (pre-increment tentative, apply only after f_open OK) */
        char filename[32];
        snprintf(filename, sizeof(filename), "REC_%03lu.wav",
                 (unsigned long)(g_FileIndex + 1u));
        strncpy(logMsg.filename, filename, sizeof(logMsg.filename));
        logMsg.filename[sizeof(logMsg.filename) - 1u] = '\0';

        /* FIL must be static + 32-byte aligned: FatFS FIL contains a 512-byte
         * DMA buffer; SDMMC IDMA requires 32-byte cache-line alignment on
         * STM32H747 with DCache.  Stack only guarantees 8-byte alignment →
         * CFSR.UNALIGNED HardFault if FIL is declared as a local variable. */
        static FIL file __attribute__((aligned(32)));
        UINT     bw;
        WavHdr_t hdr;

        FRESULT fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK)
        {
            logMsg.result = REC_ERR_SD_OPEN;
            goto done;
        }

        g_FileIndex++;   /* only increment after successful f_open */

        BuildWavHdr(&hdr, samplesSnapshot);

        fr = f_write(&file, &hdr, sizeof(hdr), &bw);
        if (fr != FR_OK || bw != sizeof(hdr))
        {
            logMsg.result = REC_ERR_SD_WRITE_HDR;
            f_close(&file);
            goto done;
        }

        /* Write PCM via AXI SRAM bounce buffer.
         * g_AudioBuf lives in SDRAM (0xD0000000). SDMMC IDMA cannot reliably
         * access SDRAM via the FMC bus on STM32H747I-DISCO — direct f_write
         * from SDRAM causes code=7 (partial or zero-byte write).
         * Fix: memcpy 4KB chunks SDRAM→AXI SRAM, then f_write from AXI SRAM. */
        {
            static uint8_t s_pcmBounce[4096u] __attribute__((aligned(32)));
            uint32_t remaining = logMsg.sizeBytes;
            uint32_t offset    = 0u;
            FRESULT  frPcm     = FR_OK;
            while (remaining > 0u && frPcm == FR_OK)
            {
                uint32_t chunk = (remaining < sizeof(s_pcmBounce)) ? remaining
                                                                    : sizeof(s_pcmBounce);
                memcpy(s_pcmBounce, (const uint8_t *)g_AudioBuf + offset, chunk);
                frPcm = f_write(&file, s_pcmBounce, chunk, &bw);
                if (frPcm != FR_OK || bw != chunk) { frPcm = FR_DISK_ERR; break; }
                offset    += chunk;
                remaining -= chunk;
            }
            if (frPcm != FR_OK || offset != logMsg.sizeBytes)
            {
                logMsg.result = REC_ERR_SD_WRITE_PCM;
                f_close(&file);
                goto done;
            }
        }

        if (f_close(&file) != FR_OK)
        {
            logMsg.result = REC_ERR_SD_CLOSE;
            goto done;
        }

        logMsg.success = 1u;

done:
        if (xQueueSend(xLogQueue, &logMsg, pdMS_TO_TICKS(100u)) != pdTRUE)
        {
            SEGGER_RTT_WriteString(0, "[REC] xLogQueue full - log dropped\r\n");
        }

        g_State = REC_IDLE;
        __DSB();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTTLogTask — prints bug log at boot, then reports WAV save results
 * ═══════════════════════════════════════════════════════════════════════════ */
void RTTLogTask(void *arg)
{
    (void)arg;

    /* Print living bug log to RTT at every boot — visible in RTT Viewer */
    SEGGER_RTT_WriteString(0, "\r\n=== BUG LOG ===\r\n");
    SEGGER_RTT_WriteString(0, "B-001 BUILD  .sdram section missing in .icf    FIXED\r\n");
    SEGGER_RTT_WriteString(0, "B-002 DMA    vTaskDelay used instead of drain  FIXED\r\n");
    SEGGER_RTT_WriteString(0, "B-003 SD     f_mount called after xTaskCreate  FIXED\r\n");
    SEGGER_RTT_WriteString(0, "=== END BUG LOG ===\r\n\r\n");

    char rttBuf[96];
    LogMsg_t msg;
    for (;;)
    {
        /* Poll SD_DETECT (PI8, active-low) every 200 ms via queue timeout.
         * LED2 (PI13) ON = card present, OFF = card absent. */
        if (xQueueReceive(xLogQueue, &msg, pdMS_TO_TICKS(200u)) != pdTRUE)
        {
            GPIO_PinState det = HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_8);
            HAL_GPIO_WritePin(GPIOI, GPIO_PIN_13,
                              (det == GPIO_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET);
            continue;
        }

        if (msg.result == REC_OK)
        {
            snprintf(rttBuf, sizeof(rttBuf),
                "[REC] OK      %s  %lu bytes  t=%lu ms\r\n",
                msg.filename,
                (unsigned long)msg.sizeBytes,
                (unsigned long)msg.timestamp);
        }
        else
        {
            snprintf(rttBuf, sizeof(rttBuf),
                "[REC] FAIL    %s  code=%d  t=%lu ms\r\n",
                msg.filename,
                (int)msg.result,
                (unsigned long)msg.timestamp);
        }
        SEGGER_RTT_WriteString(0, rttBuf);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  HealthMonTask — SD card health monitor, 10-second cadence
 *
 *  Prints to RTT channel 0 every 10 seconds:
 *    [HEALTH] card=PRESENT  free=1234 KB  used=567 KB  err=0x00000000
 *    [HEALTH] card=ABSENT
 *
 *  Checks:
 *    - SD_DETECT pin PI8 (active-low: RESET = card present)
 *    - f_getfree("0:") for free/used cluster count (only when card present
 *      and SYS_MODE_RECORD — never touches FatFS when USB-MSC owns the card)
 *    - s_hsd1.ErrorCode from audio_sd.c (exposed via AudioSD_GetErrorCode())
 *
 *  Priority 1 (lowest) — must not interfere with recording pipeline.
 * ═══════════════════════════════════════════════════════════════════════════ */
void HealthMonTask(void *arg)
{
    (void)arg;

    char buf[80];
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(10000u));

        /* SD_DETECT: PI8, active-low — RESET means card is inserted */
        GPIO_PinState det = HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_8);
        if (det != GPIO_PIN_RESET)
        {
            SEGGER_RTT_WriteString(0, "[HEALTH] card=ABSENT\r\n");
            continue;
        }

        /* Only query FatFS when FatFS owns the card */
        if (g_SysMode != SYS_MODE_RECORD)
        {
            SEGGER_RTT_WriteString(0, "[HEALTH] card=PRESENT  mode=USB-MSC\r\n");
            continue;
        }

        DWORD   freeClusters = 0u;
        FATFS  *pfs          = NULL;
        FRESULT fr           = f_getfree("0:", &freeClusters, &pfs);
        if (fr == FR_OK && pfs != NULL)
        {
            /* cluster size in sectors × 512 bytes → KB */
            uint32_t clusterKB  = (uint32_t)(pfs->csize) / 2u; /* sectors/cluster ÷ 2 = KB/cluster */
            uint32_t freeKB     = (uint32_t)(freeClusters)             * clusterKB;
            uint32_t totalKB    = (uint32_t)(pfs->n_fatent - 2u)       * clusterKB;
            uint32_t usedKB     = totalKB - freeKB;
            uint32_t errCode    = AudioSD_GetErrorCode();
            snprintf(buf, sizeof(buf),
                "[HEALTH] card=PRESENT  free=%lu KB  used=%lu KB  sdErr=0x%08lX\r\n",
                (unsigned long)freeKB,
                (unsigned long)usedKB,
                (unsigned long)errCode);
        }
        else
        {
            snprintf(buf, sizeof(buf),
                "[HEALTH] f_getfree fail: fr=%d\r\n", (int)fr);
        }
        SEGGER_RTT_WriteString(0, buf);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Private helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* StoreDmaChunk — copy one half of g_DmaBuf into g_AudioBuf.
 *
 * Which half? Derived from g_SampleCount:
 *   call 0: count=0     → (0/256)%2 = 0 → copy g_DmaBuf[0..255]   (half-complete)
 *   call 1: count=256   → (256/256)%2=1 → copy g_DmaBuf[256..511] (full-complete)
 *   call 2: count=512   → (512/256)%2=0 → copy g_DmaBuf[0..255]   (half-complete)
 *   ... alternates perfectly with DMA ping-pong.
 *
 * toCopy is capped so we never write past AUDIO_BUFFER_SAMPLES.
 */
static void StoreDmaChunk(void)
{
    if (g_SampleCount >= AUDIO_BUFFER_SAMPLES) return;

    uint8_t halfIdx = (uint8_t)((g_SampleCount / DMA_HALF_SIZE) & 1u);
    const int32_t *src = (halfIdx == 0u) ? &g_DmaBuf[0] : &g_DmaBuf[DMA_HALF_SIZE];

    uint32_t remaining = AUDIO_BUFFER_SAMPLES - g_SampleCount;
    uint32_t toCopy    = (remaining < DMA_HALF_SIZE) ? remaining : DMA_HALF_SIZE;

    for (uint32_t i = 0u; i < toCopy; i++)
    {
        /* DFSDM result: 24-bit audio in bits [23:8] of int32.
         * Right-shift by 8 extracts the 16-bit PCM value. */
        g_AudioBuf[g_SampleCount + i] = (int16_t)(src[i] >> 8);
    }
    g_SampleCount += toCopy;
}

static void BuildWavHdr(WavHdr_t *h, uint32_t nSamples)
{
    uint32_t dataBytes = nSamples * sizeof(int16_t);
    h->riff[0]='R'; h->riff[1]='I'; h->riff[2]='F'; h->riff[3]='F';
    h->chunkSize      = dataBytes + 36u;
    h->wave[0]='W'; h->wave[1]='A'; h->wave[2]='V'; h->wave[3]='E';
    h->fmt[0]='f';  h->fmt[1]='m';  h->fmt[2]='t';  h->fmt[3]=' ';
    h->subchunk1Size  = 16u;
    h->audioFormat    = 1u;                 /* PCM = uncompressed         */
    h->numChannels    = 1u;                 /* mono                       */
    h->sampleRate     = SAMPLE_RATE;        /* 16000                      */
    h->byteRate       = SAMPLE_RATE * 2u;   /* sampleRate × ch × (bps/8)  */
    h->blockAlign     = 2u;                 /* ch × (bps/8)               */
    h->bitsPerSample  = 16u;
    h->data[0]='d'; h->data[1]='a'; h->data[2]='t'; h->data[3]='a';
    h->subchunk2Size  = dataBytes;
}

static void Button_GPIO_Init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = GPIO_PIN_13;           /* PC13 = blue wakeup button    */
    gpio.Mode = GPIO_MODE_IT_RISING;   /* board: press pulls PC13 to VDD via 10k pull-down */
    gpio.Pull = GPIO_NOPULL;           /* external 10k pull-down on board — no internal pull needed */
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5u, 0u);  /* FreeRTOS-safe prio */
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

static void DFSDM_HW_Init(void)
{
    /* GPIO: PD3 = DFSDM1_CKOUT (2 MHz clock to mic)
     *       PD6 = DFSDM1_DATIN0 (PDM data from mic) */
    __HAL_RCC_GPIOD_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_3 | GPIO_PIN_6;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF3_DFSDM1;
    HAL_GPIO_Init(GPIOD, &gpio);

    __HAL_RCC_DFSDM1_CLK_ENABLE();

    /* ── Channel 0 ────────────────────────────────────────────────────── */
    s_hdfsdm_ch.Instance = DFSDM1_Channel0;
    /* Output clock: generate 2 MHz PDM clock on CKOUT (PD3) */
    s_hdfsdm_ch.Init.OutputClock.Activation = ENABLE;
    s_hdfsdm_ch.Init.OutputClock.Selection  = DFSDM_CHANNEL_OUTPUT_CLOCK_SYSTEM;
    s_hdfsdm_ch.Init.OutputClock.Divider    = 24u; /* 100MHz/(2×25) = 2 MHz */
    /* Input: external PDM on this channel's own pins */
    s_hdfsdm_ch.Init.Input.Multiplexer      = DFSDM_CHANNEL_EXTERNAL_INPUTS;
    s_hdfsdm_ch.Init.Input.DataPacking      = DFSDM_CHANNEL_STANDARD_MODE;
    s_hdfsdm_ch.Init.Input.Pins             = DFSDM_CHANNEL_SAME_CHANNEL_PINS;
    /* Serial interface: SPI rising edge (left-channel MP34DT01) */
    s_hdfsdm_ch.Init.SerialInterface.Type     = DFSDM_CHANNEL_SPI_RISING;
    s_hdfsdm_ch.Init.SerialInterface.SpiClock = DFSDM_CHANNEL_SPI_CLOCK_INTERNAL;
    /* Analog watchdog (unused, must be initialised) */
    s_hdfsdm_ch.Init.Awd.FilterOrder          = DFSDM_CHANNEL_FASTSINC_ORDER;
    s_hdfsdm_ch.Init.Awd.Oversampling         = 10u;
    s_hdfsdm_ch.Init.Offset                   = 0;
    s_hdfsdm_ch.Init.RightBitShift            = 8u;
    if (HAL_DFSDM_ChannelInit(&s_hdfsdm_ch) != HAL_OK) { Error_Handler(); }

    /* ── Filter 0 — 16 kHz output ────────────────────────────────────── */
    s_hdfsdm_flt.Instance = DFSDM1_Filter0;
    /* Regular conversion: software trigger, fast mode, DMA */
    s_hdfsdm_flt.Init.RegularParam.Trigger  = DFSDM_FILTER_SW_TRIGGER;
    s_hdfsdm_flt.Init.RegularParam.FastMode = ENABLE;
    s_hdfsdm_flt.Init.RegularParam.DmaMode  = ENABLE;
    /* Injected conversion: disabled */
    s_hdfsdm_flt.Init.InjectedParam.Trigger        = DFSDM_FILTER_SW_TRIGGER;
    s_hdfsdm_flt.Init.InjectedParam.ScanMode        = DISABLE;
    s_hdfsdm_flt.Init.InjectedParam.DmaMode         = DISABLE;
    s_hdfsdm_flt.Init.InjectedParam.ExtTrigger      = DFSDM_FILTER_EXT_TRIG_TIM1_TRGO;
    s_hdfsdm_flt.Init.InjectedParam.ExtTriggerEdge  = DFSDM_FILTER_EXT_TRIG_BOTH_EDGES;
    /* SINC3 at OSR=125: 2 MHz / 125 = 16,000 Hz */
    s_hdfsdm_flt.Init.FilterParam.SincOrder       = DFSDM_FILTER_SINC3_ORDER;
    s_hdfsdm_flt.Init.FilterParam.Oversampling    = 125u;
    s_hdfsdm_flt.Init.FilterParam.IntOversampling = 1u;
    if (HAL_DFSDM_FilterInit(&s_hdfsdm_flt) != HAL_OK) { Error_Handler(); }

    /* Link Channel 0 to Filter 0 for continuous regular conversion */
    if (HAL_DFSDM_FilterConfigRegChannel(&s_hdfsdm_flt,
                                          DFSDM_CHANNEL_0,
                                          DFSDM_CONTINUOUS_CONV_ON) != HAL_OK)
    { Error_Handler(); }
}

static void DMA_HW_Init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE();  /* safe to call again if already enabled */

    s_hdma.Instance                 = DMA1_Stream1;
    s_hdma.Init.Request             = DMA_REQUEST_DFSDM1_FLT0;  /* DMAMUX ch 101 */
    s_hdma.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;       /* DFSDM result = 32-bit */
    s_hdma.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;       /* g_DmaBuf = int32_t    */
    s_hdma.Init.Mode                = DMA_CIRCULAR;              /* ping-pong             */
    s_hdma.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;      /* direct mode           */
    if (HAL_DMA_Init(&s_hdma) != HAL_OK) { Error_Handler(); }

    __HAL_LINKDMA(&s_hdfsdm_flt, hdmaReg, s_hdma);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}
