/*
 * voice_recorder.c — 3-second button-triggered voice recorder (fixed file size)
 *
 * ── FINAL FIX SUMMARY (AN5027 + ST BSP audio_record.c alignment) ────────────
 *
 * Root cause of noise/saturation: buffer geometry did not match what the
 * PDM2PCM library expects. The library is a rigid "black box" — it requires
 * exact synchronisation between buffer size, channel count, and output samples.
 *
 * The fix aligns every parameter to the ST BSP example exactly:
 *   Reference: STM32Cube_FW_H7\Projects\STM32H747I-DISCO\Examples\BSP\CM7\Src\audio_record.c
 *
 *   PDM math (AN5027 Section 3 — must all balance simultaneously):
 *     128 words × 16 bits            = 2048 bits total per half
 *     2048 bits ÷ 2 channels         = 1024 bits per channel
 *     1024 bits ÷ 64 OSR             = 16 PCM samples  ✓
 *
 *   What was wrong before → what it is now:
 *     PDM_BUF_TOTAL : 4096 words (32× too large, filter misaligned) → 128 words
 *     PDM_BUF_HALF  : 128 words (2× too large — stereo formula on MONO stream) → 64 words
 *     in_ptr_channels : correct value is 1 (HAL PDM MONO: single D1 channel per word)
 *     PDM_Filter calls : 16× per half (wrong, broke internal history)  → 1× per half
 *     DMA cadence  : ~4 ms per half (MONO, 64 words at 1.024MHz/16 = 1ms) → correct
 *
 * ── PIPELINE ─────────────────────────────────────────────────────────────────
 *   Blue button PC13 → ISR → xTaskNotifyFromISR(voiceRecTaskHandle)
 *   VoiceRecTask wakes → HAL_SAI_Receive_DMA (SAI4 PDM mode, 128-word MONO circular)
 *   BDMA half/full callbacks (every 4 ms) → give s_dmaSem (counting, depth 4)
 *   VoiceRecTask drain loop → StoreDmaChunk() → PDM_Filter (1 call, 64 words→16 PCM)
 *   Accumulates 3000 callbacks × 16 samples = 48000 samples (3 s) in g_AudioBuf
 *   Stop DMA → xQueueSend(xVoiceQueue) → SDWriteTask wakes
 *   SDWriteTask → f_open / f_write(WAV header + PCM) / f_close → xLogQueue
 *   RTTLogTask → SEGGER_RTT result
 *
 * ── HARDWARE ─────────────────────────────────────────────────────────────────
 *   SAI4_Block_A  PDM master-receive (onboard MP34DT05-A microphone)
 *   Pins:  PE4 (AF8 =SAI4_FS_A)  → frame sync  (unlocks SAI4 master clock tree)
 *          PE5 (AF8 =SAI4_SCK_A) → serial clock (SAI4 internal bit clock)
 *          PE2 (AF10=SAI4_CK1)   → PDM clock output to microphone
 *          PC1 (AF10=SAI4_D1)    → PDM data input from microphone
 *   BDMA_Channel1 — only DMA that can reach D3 SRAM (SAI4 is in D3 domain)
 *   g_PdmBuf at 0x38000000 (D3 SRAM) — BDMA target, outside D-Cache region
 *   CRC peripheral — must be enabled before PDM_Filter_Init (library uses it)
 *   Button: PC13 EXTI15_10 rising edge  |  LED: PI12
 *
 * ── CLOCK MATH ───────────────────────────────────────────────────────────────
 *   PLL2: HSE=25MHz / M=25 × N=344 / P=7 → 49.14 MHz (SAI4A kernel clock)
 *   SAI4 AudioFreq = SAMPLE_RATE × 8 = 128000 → HAL computes MCKDIV=24
 *   PDM_CLK = 49.14 MHz / (24×2) = 1.02381 MHz  (target 1.024 MHz, 0.02% error ✓)
 *   Decimation = 64  →  PCM = 1.02381 MHz / 64 = 16,000 Hz  ✓
 */

#include "voice_recorder.h"
#include "main.h"           /* LED1_Pin, LED1_GPIO_Port, Error_Handler */
#include "stm32h7xx_hal.h"
#include "pdm2pcm_glo.h"    /* ST PDM-to-PCM filter library (SAI4 PDM → 16-bit PCM) */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "ff.h"             /* FatFS f_open/f_write/f_close */
#include "audio_sd.h"       /* AudioSD_GetErrorCode() — HealthMonTask */
#include "log_mutex.h"      /* RTT_TS() — timestamped RTT log lines  */
#include <string.h>
#include <stdio.h>       /* snprintf */
#include <limits.h>      /* ULONG_MAX */

/* ── Audio constants ─────────────────────────────────────────────────────── */
#define SAMPLE_RATE           16000u
#define RECORD_DURATION_S     3u
#define AUDIO_BUFFER_SAMPLES  (SAMPLE_RATE * RECORD_DURATION_S)  /* 48000 */
#define RECORD_MS             (RECORD_DURATION_S * 1000u)
#define DEBOUNCE_MS           300u

/* PDM buffer sizing — matched exactly to ST BSP example:
 *   Projects\STM32H747I-DISCO\Examples\BSP\CM7\Src\audio_record.c
 *
 *   PDM buffer sizing — ST BSP formula (matched to audio_record.c):
 *     SAI4 packs 8 PDM bits into each byte, with each byte duplicated across
 *     both halves of the 16-bit DMA word (confirmed: 9393 6363 E5E5 in memory).
 *     in_ptr_channels=2: library reads stride-2 bytes, skipping the duplicate.
 *     128 words × 2 bytes / stride-2 = 128 unique bytes = 1024 PDM bits → 16 PCM samples.
 *   → PDM_BUF_HALF = 128 words, PDM_BUF_TOTAL = 256 words
 *   → in_ptr_channels=2: stride-2 byte access extracts unique PDM bytes
 *   → output_samples_number = AudioFreq/1000 = 16 PCM samples per call
 *   → one PDM_Filter() call per DMA half: 128 words → 16 PCM samples
 *
 * DMA half produces 16 PCM samples per callback.
 * VoiceRecTask drain loop accumulates until AUDIO_BUFFER_SAMPLES=48000.
 * Callbacks fired per recording = 48000 / 16 = 3000.
 *
 * 0x80 risk: if PDM_Filter_setConfig rejects output_samples_number=16,
 * accumulate 4 halves (64 samples) and call once with output_samples_number=64.
 * Log line "[REC] PDM reset:" will show rc to decide.
 */
#define PDM_DEC_FACTOR           128u  /* MP34DT05-A spec: 1.2–3.25 MHz clock required.
                                        * 64→1.024 MHz (below spec min) caused noise/hiss.
                                        * 128→2.047 MHz (within spec, tested at 2.4 MHz). */
#define PDM_FILTER_CALL_SAMPLES  (SAMPLE_RATE / 1000u)              /* 16 */
#define PDM_BUF_HALF             (PDM_FILTER_CALL_SAMPLES * PDM_DEC_FACTOR / 16u)       /* 128: words per half */
#define PDM_BUF_TOTAL            (PDM_BUF_HALF * 2u)                /* 256: full DMA circular buffer */
#define DMA_HALF_SIZE            PDM_FILTER_CALL_SAMPLES             /* 16: PCM samples per DMA half */
#define WARMUP_CALLS             300u                                /* discard first 300 DMA halves (300 ms) */

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
static SAI_HandleTypeDef    s_hsai;
static DMA_HandleTypeDef    s_hdma;
static PDM_Filter_Handler_t s_pdmHandler;
static PDM_Filter_Config_t  s_pdmConfig;

/* ── PDM DMA buffer — MUST be in D3 SRAM (0x38000000, 64 KB).
 * SAI4 uses BDMA (Basic DMA) which can only access D3 SRAM.
 * Placed at absolute address via IAR pragma; __no_init suppresses
 * zero-init (linker cannot initialize a fixed-address static buffer). */
#pragma location = 0x38000000
static __no_init uint16_t g_PdmBuf[PDM_BUF_TOTAL];

/* Audio accumulation buffer — 3 s × 16000 Hz × 2 bytes = 96 KB in AXI SRAM.
 * Previously in SDRAM (.sdram_bss) which shares AHB3 with SDMMC1 IDMA —
 * FMC auto-refresh every 7.8 µs caused SDMMC FIFO overrun (RXOVERR) during
 * disk_write. Moved to AXI SRAM (default .bss, 0x24xxxxxx) which is on the
 * AXI bus — no FMC contention, 254 KB free in this region. */
int16_t g_AudioBuf[AUDIO_BUFFER_SAMPLES]
    __attribute__((aligned(32)));

/* No staging buffer needed — in_ptr_channels=2, library reads stride-2 bytes to
 * extract the unique PDM byte from each duplicated 16-bit word. Feed g_PdmBuf directly. */

/* ── Recording state ─────────────────────────────────────────────────────── */
volatile RecState_t  g_State       = REC_IDLE;
volatile uint32_t    g_SampleCount = 0u;
static   uint32_t    g_DmaCallCount = 0u;  /* total DMA halves consumed — used for ping-pong halfIdx */
static   uint32_t    g_FileIndex   = 0u;

/* ── FreeRTOS objects ────────────────────────────────────────────────────── */
TaskHandle_t       voiceRecTaskHandle = NULL;
QueueHandle_t      xLogQueue          = NULL;

/* ── DMA sync measurement ────────────────────────────────────────────────────
 * Each queue entry carries the buffer pointer AND the DWT cycle count at the
 * moment the ISR posted it.  The drain loop measures how long the entry sat in
 * the queue (latency = pop_cycles - post_cycles).  This is the software
 * equivalent of the oscilloscope pin-toggle test described in the sync article:
 *
 *   ISR posts  → DWT stamp = "pin HIGH"
 *   Task pops  → delta     = "pin LOW"  (= CPU latency to service this half)
 *
 * If latency > 1 ms (480,000 cycles at 480 MHz) the DMA is lapping the CPU.
 * ─────────────────────────────────────────────────────────────────────────── */
typedef struct {
    const uint16_t *ptr;       /* buffer pointer (&g_PdmBuf[0] or [PDM_BUF_HALF]) */
    uint32_t        stamp_cy;  /* DWT->CYCCNT at ISR post time */
} DmaEntry_t;

static QueueHandle_t s_dmaQueue = NULL;
static volatile uint32_t s_dmaQueueOverflow = 0u;  /* ISR drop counter */

/* Sync statistics — reset each recording, reported in quality log */
static uint32_t s_latency_max_cy  = 0u;  /* worst-case ISR→task latency (cycles) */
static uint32_t s_latency_max_us  = 0u;  /* same, in microseconds                */
static UBaseType_t s_qDepth_max   = 0u;  /* highest queue depth seen mid-drain   */
static uint32_t s_desync_count    = 0u;  /* halves where latency > 1 ms deadline */

/* Audio quality results — static so IAR Watch window can see them after recording */
static uint32_t s_noise_rms   = 0u;
static uint32_t s_speech_rms  = 0u;
static int32_t  s_snr_db      = 0;
static int32_t  s_peak        = 0;
static int32_t  s_dc_offset   = 0;
static uint32_t s_clip_cnt    = 0u;

/* ── LED helpers ─────────────────────────────────────────────────────────── */
#define LED_ON()     HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET)
#define LED_OFF()    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET)
#define LED_TOGGLE() HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)

/* ── Forward declarations ────────────────────────────────────────────────── */
static void Button_GPIO_Init(void);
static void SAI4_HW_Init(void);
static void DMA_HW_Init(void);
static void StoreDmaChunk(const uint16_t *pdmSrc);
static void BuildWavHdr(WavHdr_t *h, uint32_t nSamples);
static void AudioQuality_Report(void);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════════ */

RecState_t VoiceRec_GetState(void)
{
    return g_State;
}

/* Lightweight init — button GPIO + NVIC only.
 * Safe to call without touching SAI4 or DMA.
 * Use this during bring-up to verify button + RTT before enabling recording. */
void VoiceRec_ButtonInit(void)
{
    g_State   = REC_IDLE;
    __DSB();
    Button_GPIO_Init();
}

void VoiceRec_Init(void)
{
    /* Create FreeRTOS objects before enabling any IRQs */
    s_dmaQueue = xQueueCreate(32u, sizeof(DmaEntry_t));
    xLogQueue  = xQueueCreate(4u, sizeof(LogMsg_t));
    configASSERT(s_dmaQueue);
    configASSERT(xLogQueue);

    Button_GPIO_Init();
    SAI4_HW_Init();
    DMA_HW_Init();

    /* PDM filter library init — requires CRC peripheral (already enabled by MX_CRC_Init) */
    s_pdmHandler.bit_order        = PDM_FILTER_BIT_ORDER_MSB;  /* BSP reference: MSB with SAI_FIRSTBIT_LSB */
    s_pdmHandler.endianness       = PDM_FILTER_ENDIANNESS_LE;
    s_pdmHandler.high_pass_tap    = 2122358088u;  /* standard HP filter coefficient */
    s_pdmHandler.in_ptr_channels  = 1u;   /* 1: stride-1. SAI4 MONO: both bytes of each 16-bit word are identical (confirmed: D5D5 3838...). D0=D1 in MONO mode. */
    s_pdmHandler.out_ptr_channels = 1u;   /* 1: mono PCM output */
    if (PDM_Filter_Init(&s_pdmHandler) != 0u) { Error_Handler(); }

    s_pdmConfig.decimation_factor     = PDM_FILTER_DEC_FACTOR_128;
    s_pdmConfig.output_samples_number = PDM_FILTER_CALL_SAMPLES;  /* 16: BSP formula AudioFreq/1000 */
    s_pdmConfig.mic_gain              = 24;  /* 24 dB — per AN5027 + CLAUDE.md spec */
    {
        uint32_t rc = PDM_Filter_setConfig(&s_pdmHandler, &s_pdmConfig);
        RLOG("[REC] PDM_Filter_setConfig rc=0x%lX (0=OK, 0x80=samples_err)", (unsigned long)rc);
        if (rc != 0u) { Error_Handler(); }
    }

    g_State       = REC_IDLE;
    g_SampleCount = 0u;

    g_FileIndex = 0u;  /* SDWriteTask will scan SD on first use */

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
    if (g_State == REC_IDLE
        && voiceRecTaskHandle != NULL)
    {
        BaseType_t higher = pdFALSE;
        xTaskNotifyFromISR(voiceRecTaskHandle, 1u, eSetBits, &higher);
        portYIELD_FROM_ISR(higher);
    }
}

/* SAI4 BDMA half-complete — g_PdmBuf[0..PDM_BUF_HALF-1] ready.
 * Stamp DWT cycle counter at ISR entry — this is "pin HIGH" in the article. */
void HAL_SAI_RxHalfCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance != SAI4_Block_A) return;
    DmaEntry_t e = { &g_PdmBuf[0], DWT->CYCCNT };
    BaseType_t higher = pdFALSE;
    if (xQueueSendFromISR(s_dmaQueue, &e, &higher) != pdTRUE)
        s_dmaQueueOverflow++;
    portYIELD_FROM_ISR(higher);
}

/* SAI4 BDMA full-complete — g_PdmBuf[PDM_BUF_HALF..PDM_BUF_TOTAL-1] ready. */
void HAL_SAI_RxCpltCallback(SAI_HandleTypeDef *hsai)
{
    if (hsai->Instance != SAI4_Block_A) return;
    DmaEntry_t e = { &g_PdmBuf[PDM_BUF_HALF], DWT->CYCCNT };
    BaseType_t higher = pdFALSE;
    if (xQueueSendFromISR(s_dmaQueue, &e, &higher) != pdTRUE)
        s_dmaQueueOverflow++;
    portYIELD_FROM_ISR(higher);
}

/* IRQ trampoline — called from BDMA_Channel1_IRQHandler in stm32h7xx_it.c */
void VoiceRec_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  VoiceRecTask — wakes on trigger, active-drains DMA for 3 s (fixed), signals SD
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

        g_SampleCount        = 0u;
        g_DmaCallCount       = 0u;
        s_dmaQueueOverflow   = 0u;
        s_latency_max_cy     = 0u;
        s_latency_max_us     = 0u;
        s_qDepth_max         = 0u;
        s_desync_count       = 0u;
        g_State              = REC_RECORDING;
        __DSB();
        RLOG("[REC] --- Stage 0: recording 3s ---\r\n");

        /* Reset PDM filter internal state before each recording. */
        {
            uint32_t r1 = PDM_Filter_Init(&s_pdmHandler);
            uint32_t r2 = PDM_Filter_setConfig(&s_pdmHandler, &s_pdmConfig);
            RLOG("[REC] PDM reset: Init=0x%lX setConfig=0x%lX samples=%u",
                 (unsigned long)r1, (unsigned long)r2,
                 (unsigned)s_pdmConfig.output_samples_number);
        }

        /* Start SAI4 PDM DMA (BDMA_Channel1 → g_PdmBuf in D3 SRAM) */
        HAL_StatusTypeDef hal =
            HAL_SAI_Receive_DMA(&s_hsai,
                                (uint8_t *)g_PdmBuf,
                                PDM_BUF_TOTAL);
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
        /* Always fill the complete buffer (48000 samples = 3 s).           */
        /* Button is a start trigger only — file size is always fixed.      */

        /* Queue-based drain: each DMA callback posts its buffer pointer + DWT stamp.
         * The drain loop pops in FIFO order — always the correct half, in order.
         *
         * Sync measurement (software oscilloscope):
         *   ISR stamps DWT at post  = "pin HIGH"
         *   Task reads DWT at pop   = "pin LOW"
         *   delta = latency = time the half sat waiting for the CPU
         *   If delta > 480,000 cycles (1 ms @ 480 MHz) → desync */
        TickType_t ledToggle = xTaskGetTickCount() + pdMS_TO_TICKS(500u);

#define CYCLES_PER_MS  480000u   /* 480 MHz */
#define CYCLES_PER_US  480u

        while (g_SampleCount < AUDIO_BUFFER_SAMPLES)
        {
            if (xTaskGetTickCount() >= ledToggle)
            {
                LED_TOGGLE();
                ledToggle += pdMS_TO_TICKS(500u);
            }

            DmaEntry_t e = { NULL, 0u };
            if (xQueueReceive(s_dmaQueue, &e, pdMS_TO_TICKS(10u)) == pdTRUE)
            {
                /* "pin LOW" — measure how long this half waited */
                uint32_t now     = DWT->CYCCNT;
                uint32_t latency = now - e.stamp_cy;   /* wraps correctly */

                if (latency > s_latency_max_cy)
                {
                    s_latency_max_cy = latency;
                    s_latency_max_us = latency / CYCLES_PER_US;
                }
                if (latency > CYCLES_PER_MS)
                    s_desync_count++;

                /* Queue depth at this moment = backlog of unprocessed halves */
                UBaseType_t depth = uxQueueMessagesWaiting(s_dmaQueue);
                if (depth > s_qDepth_max)
                    s_qDepth_max = depth;

                StoreDmaChunk(e.ptr);
            }
        }

        /* Stop SAI4 DMA */
        HAL_SAI_DMAStop(&s_hsai);

        /* Drain any stale queue entries from callbacks that fired after stop */
        {
            DmaEntry_t discard = { NULL, 0u };
            while (xQueueReceive(s_dmaQueue, &discard, 0) == pdTRUE) {}
        }

        /* Flush SINC3 filter state accumulated over the 3s recording.
         * pInternalMemory holds 3s of history — must be cleared now so the
         * next recording starts with a clean filter, not stale history. */
        PDM_Filter_Init(&s_pdmHandler);

        /* Audio quality report — Terminal I/O only (IAR debugger window) */
        AudioQuality_Report();

        /* Safety: if SAI4 DMA gave no samples, log and skip SD write */
        if (g_SampleCount == 0u)
        {
            RLOG("[REC] WARN: SAI4 DMA gave 0 samples — check SAI4/BDMA config");
            g_State = REC_IDLE; __DSB();
            continue;
        }

        RLOG("[REC] Recording done: samples=%lu\r\n", (unsigned long)g_SampleCount);
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
    bool s_indexScanned = false;

    for (;;)
    {
        xQueueReceive(q, &msg, portMAX_DELAY);

        /* Scan SD once per boot to find the next available file index.
         * Done here (not in VoiceRec_Init) because SD is mounted by now. */
        if (!s_indexScanned)
        {
            s_indexScanned = true;
            g_FileIndex = 0u;
            char probe[32];
            for (uint32_t i = 1u; i <= 999u; i++)
            {
                snprintf(probe, sizeof(probe), "REC_%03lu.wav", (unsigned long)i);
                FILINFO fno;
                if (f_stat(probe, &fno) != FR_OK) break;
                g_FileIndex = i;
            }
            RLOG("[REC] SD scan: next index=%lu", (unsigned long)(g_FileIndex + 1u));
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

        RLOG("[REC] --- Stage 1: writing WAV to SD ---");
        RLOG("[REC] file=%s  samples=%lu",
            filename, (unsigned long)samplesSnapshot);

        FRESULT fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK)
        {
            /* Blank / unrecognised card — format first, then retry */
            if (fr == FR_NO_FILESYSTEM)
            {
                RLOG("[REC] FAIL f_open: no filesystem — formatting card");
                if (!AudioSD_Format())
                {
                    logMsg.result = REC_ERR_SD_OPEN;
                    goto done;
                }
            }
            else
            {
                /* SDMMC peripheral error — remount once as recovery */
                RLOG("[REC] FAIL f_open: fr=%d — remounting", (int)fr);
                if (!AudioSD_Remount())
                {
                    logMsg.result = REC_ERR_SD_OPEN;
                    goto done;
                }
            }
            fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
            if (fr != FR_OK)
            {
                RLOG("[REC] FAIL f_open after recovery: fr=%d", (int)fr);
                logMsg.result = REC_ERR_SD_OPEN;
                goto done;
            }
        }
        RLOG("[REC] f_open OK\r\n");

        g_FileIndex++;   /* only increment after successful f_open */

        BuildWavHdr(&hdr, samplesSnapshot);

        fr = f_write(&file, &hdr, sizeof(hdr), &bw);
        if (fr != FR_OK || bw != sizeof(hdr))
        {
            RLOG("[REC] FAIL f_write header: fr=%d\r\n", (int)fr);
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
                RLOG("[REC] FAIL f_write PCM: fr=%d offset=%lu\r\n",
                       (int)frPcm, (unsigned long)offset);
                logMsg.result = REC_ERR_SD_WRITE_PCM;
                f_close(&file);
                goto done;
            }
        }

        if (f_close(&file) != FR_OK)
        {
            RLOG("[REC] FAIL f_close\r\n");
            logMsg.result = REC_ERR_SD_CLOSE;
            goto done;
        }

        logMsg.success = 1u;
        RLOG("[REC] ========================================");
        RLOG("[REC] WAV WRITE COMPLETE");
        RLOG("[REC] file=%s  size=%lu bytes",
            logMsg.filename, (unsigned long)logMsg.sizeBytes);
        RLOG("[REC] ========================================");

        /* ── Stage 2: stream WAV to NORA over UART8 ────────────────────────
         * AudioSD_SendFileToUART() sends:
         *   1. ASCII header:  AUDIO:FILE:<filename>:<bytes>\n
         *   2. Raw WAV bytes: streamed in chunks until EOF
         * NORA receives the stream, uploads to GCS, runs Speech-to-Text.
         * Retry up to 3 times on failure — file stays on SD card so no
         * re-recording is needed. 2-second gap between attempts gives NORA
         * time to close the failed HTTP connection and reset. */
        {
            bool sent = false;
            RLOG("[REC] --- Stage 2: streaming to NORA ---");
            for (int attempt = 1; attempt <= 3 && !sent; attempt++)
            {
                RLOG("[REC] UART send attempt %d/3...\r\n", attempt);
                sent = AudioSD_SendFileToUART(filename);
                if (!sent && attempt < 3)
                    vTaskDelay(pdMS_TO_TICKS(2000));
            }
            if (!sent)
                RLOG("[REC] All 3 send attempts FAILED\r\n");
            else
                RLOG("[REC] Stream OK\r\n");
        }

done:
        /* Remount on any error path — SDMMC stays dirty after a write failure
         * and all subsequent FatFS calls (including HealthMonTask f_getfree)
         * return FR_NOT_READY until the peripheral is reset.
         * B-010: second occurrence 2026-04-04 → fix applied. */
        RLOG("[REC] DONE result=%d file=%s\r\n",
               (int)logMsg.result, logMsg.filename);
        if (logMsg.result != REC_OK)
            AudioSD_Remount();

        if (xQueueSend(xLogQueue, &logMsg, pdMS_TO_TICKS(100u)) != pdTRUE)
        {
            RLOG("[REC] xLogQueue full - log dropped");
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

    /* Print living bug log to RTT only (not Terminal I/O) */
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
        RTT_TS(rttBuf);
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
 *    - f_getfree("0:") for free/used cluster count (only when card present)
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
            RLOG("[HEALTH] card=ABSENT");
            continue;
        }

        /* Skip f_getfree while SD is busy (writing or streaming) — concurrent
         * FatFS access corrupts win[] sector cache → FR_DISK_ERR mid-read (B-008) */
        if (AudioSD_IsBusy())
        {
            RLOG("[HEALTH] card=PRESENT  SD busy — skipping f_getfree");
            continue;
        }

        DWORD   freeClusters = 0u;
        FATFS  *pfs          = NULL;
        FRESULT fr           = f_getfree("0:", &freeClusters, &pfs);
        if (fr == FR_NOT_READY)
        {
            /* SDMMC CPSM degraded after init — full recover and retry once */
            RLOG("[HEALTH] SD NOT_READY — remounting");
            AudioSD_Remount();
            fr = f_getfree("0:", &freeClusters, &pfs);
        }
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
        RTT_TS(buf);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Private helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

/* StoreDmaChunk — convert one DMA half (PDM) to PCM and store in g_AudioBuf.
 *
 * pdmSrc is passed by the drain loop — always the correct half:
 *   half-complete callback → &g_PdmBuf[0]         (first  128 words)
 *   full-complete callback → &g_PdmBuf[PDM_BUF_HALF] (second 128 words)
 *
 * This eliminates the g_DmaCallCount ping-pong counter which could drift
 * if semaphore tokens accumulated, causing both calls to read the same half
 * and producing a periodic 500 Hz tonal artifact.
 *
 * PDM_Filter: 64 words (128 bytes) → 16 PCM samples (in_ptr_channels=1, stride-1).
 * SAI4 MONO mode: all 128 bytes are D1 mic data (no interleaving in MONO mode).
 * Library reads all 128 bytes = 1024 PDM bits → 16 PCM samples. ✓
 */
static void StoreDmaChunk(const uint16_t *pdmSrc)
{
    if (g_SampleCount >= AUDIO_BUFFER_SAMPLES) return;

    g_DmaCallCount++;

    /* PDM → PCM: AN5027 §3.1 + BSP_AUDIO_IN_PDMToPCM pattern.
     * SAI4 MONO mode: both bytes of each 16-bit word are identical (e.g. D5D5 3838).
     * in_ptr_channels=1 (stride-1) reads all 128 bytes = 1024 PDM bits → 16 PCM samples. */
    static int16_t s_pcmHalf[DMA_HALF_SIZE];
    PDM_Filter((uint8_t *)pdmSrc, (void *)s_pcmHalf, &s_pdmHandler);

    /* Skip first WARMUP_CALLS results — SINC3 transient */
    if (g_DmaCallCount <= WARMUP_CALLS) return;

    uint32_t remaining = AUDIO_BUFFER_SAMPLES - g_SampleCount;
    uint32_t toCopy    = (remaining < DMA_HALF_SIZE) ? remaining : DMA_HALF_SIZE;

    for (uint32_t i = 0u; i < toCopy; i++)
    {
        g_AudioBuf[g_SampleCount + i] = s_pcmHalf[i];
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

    /* Joystick CENTER = PK2, active-low (47K pull-up to +3V3, schematic Sheet 10).
     * Polled in VoiceRecTask idle loop — no EXTI needed. */
    __HAL_RCC_GPIOK_CLK_ENABLE();
    gpio.Pin  = GPIO_PIN_2;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;           /* belt-and-suspenders: board already has 47K */
    HAL_GPIO_Init(GPIOK, &gpio);
}

static void SAI4_HW_Init(void)
{
    /* ── PLL2 clock for SAI4 ─────────────────────────────────────────────
     * HSE=25MHz / PLL2M=25 → VCO_in=1MHz × PLL2N=344 → VCO=344MHz / PLL2P=7
     * → PLL2P_out ≈ 49.14 MHz  (SAI4A kernel clock)
     * AudioFreq = 16000×8 = 128kHz  →  HAL computes MCKDIV for PDM clock */
    RCC_PeriphCLKInitTypeDef rcc_pdm = {0};
    HAL_RCCEx_GetPeriphCLKConfig(&rcc_pdm);
    rcc_pdm.PeriphClockSelection      = RCC_PERIPHCLK_SAI4A;
    rcc_pdm.Sai4AClockSelection       = RCC_SAI4ACLKSOURCE_PLL2;
    rcc_pdm.PLL2.PLL2M                = 25u;
    rcc_pdm.PLL2.PLL2N                = 344u;
    rcc_pdm.PLL2.PLL2P                = 7u;
    rcc_pdm.PLL2.PLL2Q                = 1u;
    rcc_pdm.PLL2.PLL2R                = 1u;
    rcc_pdm.PLL2.PLL2RGE              = RCC_PLL2VCIRANGE_1; /* VCO_in = 1 MHz → range 1-2MHz */
    rcc_pdm.PLL2.PLL2VCOSEL           = RCC_PLL2VCOWIDE;
    rcc_pdm.PLL2.PLL2FRACN            = 0u;
    if (HAL_RCCEx_PeriphCLKConfig(&rcc_pdm) != HAL_OK) { Error_Handler(); }

    /* ── GPIO ────────────────────────────────────────────────────────────
     * PE4 (AF8 =SAI4_FS_A)  — frame sync (required even in PDM mode)
     * PE5 (AF8 =SAI4_SCK_A) — serial clock (SAI4 internal bit clock)
     * PE2 (AF10=SAI4_CK1)   — PDM clock output to microphone
     * PC1 (AF10=SAI4_D1)    — PDM data input from microphone (onboard mic DOUT) */
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;

    /* PE4 (AF8=SAI4_FS_A)  — frame sync
     * PE5 (AF8=SAI4_SCK_A) — serial clock (SAI4 internal bit clock)
     * PE2 (AF10=SAI4_CK1)  — PDM clock output to microphone
     * All three are on GPIOE; BSP configures all four pins for SAI4 PDM mode. */
    gpio.Pin       = GPIO_PIN_4 | GPIO_PIN_5;
    gpio.Alternate = GPIO_AF8_SAI4;
    HAL_GPIO_Init(GPIOE, &gpio);

    gpio.Pin       = GPIO_PIN_2;
    gpio.Alternate = GPIO_AF10_SAI4;
    HAL_GPIO_Init(GPIOE, &gpio);

    /* PC1: AF10 = SAI4_D1 — PDM data input from microphone.
     * Pull-up: mic DOUT is weakly driven; pull-up prevents floating. */
    gpio.Pin       = GPIO_PIN_1;
    gpio.Pull      = GPIO_PULLUP;
    gpio.Alternate = GPIO_AF10_SAI4;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* ── SAI4 PDM init ───────────────────────────────────────────────────
     * SAI4_Block_A in PDM master-receive mode.
     * AudioFrequency = SampleRate × 8 = 128000 (HAL uses this for MCKDIV).
     * FrameLength=16, SlotNumber=1, SlotActive=0: matches ST BSP MX_SAI4_Block_A_Init exactly.
     * In SAI PDM mode the hardware captures both D0+D1 within one 16-bit slot automatically.
     * PdmInit: Activation=ENABLE, MicPairsNbr=1, ClockEnable=CLOCK1. */
    __HAL_RCC_SAI4_CLK_ENABLE();

    s_hsai.Instance = SAI4_Block_A;

    s_hsai.Init.AudioMode         = SAI_MODEMASTER_RX;
    s_hsai.Init.Synchro           = SAI_ASYNCHRONOUS;
    s_hsai.Init.SynchroExt        = SAI_SYNCEXT_DISABLE;
    s_hsai.Init.OutputDrive       = SAI_OUTPUTDRIVE_DISABLE;
    s_hsai.Init.NoDivider         = SAI_MASTERDIVIDER_DISABLE;
    s_hsai.Init.FIFOThreshold     = SAI_FIFOTHRESHOLD_1QF;
    s_hsai.Init.AudioFrequency    = (uint32_t)(SAMPLE_RATE * 16u); /* 256000 → MCKDIV=12 → PDM clock=2.047 MHz (within MP34DT05-A spec 1.2–3.25 MHz) */
    s_hsai.Init.Mckdiv            = 0u;
    s_hsai.Init.MonoStereoMode    = SAI_STEREOMODE;
    s_hsai.Init.CompandingMode    = SAI_NOCOMPANDING;
    s_hsai.Init.TriState          = SAI_OUTPUT_RELEASED;
    s_hsai.Init.Protocol          = SAI_FREE_PROTOCOL;
    s_hsai.Init.DataSize          = SAI_DATASIZE_16;
    s_hsai.Init.FirstBit          = SAI_FIRSTBIT_LSB;
    s_hsai.Init.ClockStrobing     = SAI_CLOCKSTROBING_FALLINGEDGE;

    s_hsai.Init.PdmInit.Activation  = ENABLE;
    s_hsai.Init.PdmInit.MicPairsNbr = 1u;
    s_hsai.Init.PdmInit.ClockEnable = SAI_PDM_CLOCK1_ENABLE;

    s_hsai.FrameInit.FrameLength       = 16u;  /* 1 slot × 16 bits — BSP reference: MX_SAI4_Block_A_Init */
    s_hsai.FrameInit.ActiveFrameLength = 1u;
    s_hsai.FrameInit.FSDefinition      = SAI_FS_STARTFRAME;
    s_hsai.FrameInit.FSPolarity        = SAI_FS_ACTIVE_HIGH;
    s_hsai.FrameInit.FSOffset          = SAI_FS_FIRSTBIT;

    s_hsai.SlotInit.FirstBitOffset = 0u;
    s_hsai.SlotInit.SlotSize       = SAI_SLOTSIZE_DATASIZE;
    s_hsai.SlotInit.SlotNumber     = 1u;             /* BSP reference: SlotNumber=1 in PDM mode */
    s_hsai.SlotInit.SlotActive     = SAI_SLOTACTIVE_0;  /* BSP reference: SAI_SLOTACTIVE_0 in PDM mode */

    if (HAL_SAI_Init(&s_hsai) != HAL_OK) { Error_Handler(); }
}

static void DMA_HW_Init(void)
{
    /* SAI4 uses BDMA (Basic DMA), not DMA1/DMA2.
     * BDMA can only access D3 SRAM (0x38000000) — hence g_PdmBuf placement. */
    __HAL_RCC_BDMA_CLK_ENABLE();

    s_hdma.Instance                 = BDMA_Channel1;
    s_hdma.Init.Request             = BDMA_REQUEST_SAI4_A;
    s_hdma.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD; /* SAI4 data = 16-bit */
    s_hdma.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD; /* g_PdmBuf = uint16_t */
    s_hdma.Init.Mode                = DMA_CIRCULAR;            /* ping-pong */
    s_hdma.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_hdma) != HAL_OK) { Error_Handler(); }

    __HAL_LINKDMA(&s_hsai, hdmarx, s_hdma);

    HAL_NVIC_SetPriority(BDMA_Channel1_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(BDMA_Channel1_IRQn);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  AudioQuality_Report — diagnostic measurements after each recording.
 *  Output goes to Terminal I/O only (IAR debugger window) via LOG().
 *
 *  Three measurement points:
 *
 *  [1] PDM bit density — proves the mic is producing a valid PDM bitstream.
 *      Scans entire g_PdmBuf (last 2 DMA half-buffers captured).
 *      Healthy PDM at silence: ~50% ones. Stuck/dead: 0% or 100%.
 *
 *  [2] PCM statistics — measures quality of the converted audio.
 *      noise_window : samples [0..8000)     = first 0.5 s (before speech)
 *      speech_window: samples [8000..40000) = middle 2.0 s (speech content)
 *
 *  [3] SAI4 + BDMA registers — confirms peripheral state after recording.
 *      SAI4_Block_A CR1, SR, PDMCR; BDMA Channel1 CCR, CNDTR.
 * ═══════════════════════════════════════════════════════════════════════════ */
static void AudioQuality_Report(void)
{
    /* ── [1] PDM bit density — MONO stream (all words = D1, our mic) ───────── */
    /* HAL PDM MONO mode: buffer contains only D1 data (PC1, SAI4_D1).
     * Healthy PDM at silence: ~50% ones. Stuck/dead: 0% or 100%. */
    uint32_t ones_total = 0u;
    for (uint32_t w = 0u; w < PDM_BUF_TOTAL; w++)
    {
        uint32_t v = g_PdmBuf[w];
        while (v) { ones_total++; v &= v - 1u; }
    }
    uint32_t total_bits  = PDM_BUF_TOTAL * 16u;
    uint32_t density_mic = (ones_total * 100u) / total_bits;

    RLOG("-------- Audio Quality Report --------");
    RLOG("[PDM]  raw[0..7] : %04X %04X %04X %04X %04X %04X %04X %04X",
        g_PdmBuf[0], g_PdmBuf[1], g_PdmBuf[2], g_PdmBuf[3],
        g_PdmBuf[4], g_PdmBuf[5], g_PdmBuf[6], g_PdmBuf[7]);
    RLOG("[PDM]  MONO D1 (PC1) density=%lu%%   (MONO mode: all words = mic D1)",
        (unsigned long)density_mic);
    RLOG("[PDM]  D1: %s  <-- mic is on D1 (PC1), HAL MONO mode",
        (density_mic < 5u)  ? "DEAD/stuck-LOW" :
        (density_mic > 95u) ? "DEAD/stuck-HIGH" :
        (density_mic < 40u || density_mic > 60u) ? "WARN: off-centre" : "OK ~50%");
    RLOG("[DMA]  callbacks=%lu  warmup=%u  stored=%lu  overflow=%lu%s",
        (unsigned long)g_DmaCallCount, (unsigned)WARMUP_CALLS,
        (unsigned long)g_SampleCount / DMA_HALF_SIZE,
        (unsigned long)s_dmaQueueOverflow,
        (s_dmaQueueOverflow > 0u) ? " <-- QUEUE OVERFLOW: audio gaps!" : " OK");
    /* ── Sync report (software oscilloscope) ────────────────────────────────
     * latency_max = worst-case time from ISR post to task pop  ("pin HIGH" time)
     * deadline    = 1 ms = 480,000 cycles  (DMA fires every 1 ms)
     * headroom    = deadline - latency_max  (positive = safe)
     * desync      = halves where latency > 1 ms  (should be 0)
     * ─────────────────────────────────────────────────────────────────────── */
    {
        int32_t headroom_us = (int32_t)(1000u) - (int32_t)(s_latency_max_us);
        const char *sync_status =
            (s_desync_count == 0u && s_latency_max_us < 500u)  ? "IN SYNC  (>50% headroom)" :
            (s_desync_count == 0u && s_latency_max_us < 800u)  ? "SLIGHT DRIFT (SD slow?)" :
            (s_desync_count == 0u && s_latency_max_us < 1000u) ? "NEAR LIMIT   (beep risk)" :
                                                                  "DESYNC!      (beep likely)";
        RLOG("[SYNC] latency_max=%lu us  headroom=%ld us  qDepth_max=%u  desync=%lu  → %s",
            (unsigned long)s_latency_max_us,
            (long)headroom_us,
            (unsigned)s_qDepth_max,
            (unsigned long)s_desync_count,
            sync_status);
    }

    /* ── [2] PCM statistics ──────────────────────────────────────────────── */
    /* Show samples from settled region (skip first 4 halves = 64 samples to avoid residual transient) */
    RLOG("[PCM]  out[64..71] : %d %d %d %d %d %d %d %d",
        (int)g_AudioBuf[64], (int)g_AudioBuf[65],
        (int)g_AudioBuf[66], (int)g_AudioBuf[67],
        (int)g_AudioBuf[68], (int)g_AudioBuf[69],
        (int)g_AudioBuf[70], (int)g_AudioBuf[71]);

    /* Noise window: start at sample 3200 (skip first 200ms of residual SINC3 transient), measure 0.5 s */
    uint32_t noise_start = 3200u;
    uint32_t noise_end   = (g_SampleCount < 11200u) ? g_SampleCount : 11200u;
    uint32_t noise_n     = (noise_end > noise_start) ? (noise_end - noise_start) : 0u;
    int64_t  noise_ss    = 0;   /* sum of squares */
    for (uint32_t i = noise_start; i < noise_end; i++)
    {
        int64_t s = g_AudioBuf[i];
        noise_ss += s * s;
    }
    /* Speech window: 0.5–2.5 s */
    uint32_t sp_start  = 8000u;
    uint32_t sp_end    = (g_SampleCount < 40000u) ? g_SampleCount : 40000u;
    int64_t  sp_ss     = 0;
    int64_t  dc_sum    = 0;
    int32_t  sp_peak   = 0;
    uint32_t clip_cnt  = 0u;
    for (uint32_t i = sp_start; i < sp_end; i++)
    {
        int32_t s = g_AudioBuf[i];
        dc_sum  += s;
        sp_ss   += (int64_t)s * s;
        int32_t a = (s < 0) ? -s : s;
        if (a > sp_peak) sp_peak = a;
        if (a >= 32767)  clip_cnt++;
    }
    uint32_t sp_n = (sp_end > sp_start) ? (sp_end - sp_start) : 1u;

    /* Integer square root (Newton–Raphson, converges in < 20 iterations) */
    if (noise_n > 0u)
    {
        uint64_t v = (uint64_t)(noise_ss / (int64_t)noise_n);
        uint64_t x = v;
        uint64_t y = (v > 0u) ? (v / 2u + 1u) : 0u;
        while (y < x) { x = y; y = (y + v / y) / 2u; }
        s_noise_rms = (uint32_t)x;
    }
    {
        uint64_t v = (uint64_t)(sp_ss / (int64_t)sp_n);
        uint64_t x = v;
        uint64_t y = (v > 0u) ? (v / 2u + 1u) : 0u;
        while (y < x) { x = y; y = (y + v / y) / 2u; }
        s_speech_rms = (uint32_t)x;
    }
    s_dc_offset = (int32_t)(dc_sum / (int64_t)sp_n);
    s_peak      = sp_peak;
    s_clip_cnt  = clip_cnt;

    /* SNR ≈ 6 dB per bit of log2 ratio (±3 dB accuracy — sufficient for diagnostics) */
    s_snr_db = 0;
    if (s_noise_rms > 0u && s_speech_rms > 0u)
    {
        uint32_t b_sp = 0u, b_ns = 0u, tmp;
        tmp = s_speech_rms; while (tmp > 1u) { tmp >>= 1u; b_sp++; }
        tmp = s_noise_rms;  while (tmp > 1u) { tmp >>= 1u; b_ns++; }
        s_snr_db = (int32_t)(b_sp - b_ns) * 6;
    }

    RLOG("[PCM]  noise_rms=%-6lu  speech_rms=%-6lu  snr~%+d dB",
        (unsigned long)s_noise_rms, (unsigned long)s_speech_rms, (int)s_snr_db);
    RLOG("[PCM]  peak=%-6d  dc_offset=%-6d  clip=%lu",
        (int)s_peak, (int)s_dc_offset, (unsigned long)s_clip_cnt);
    RLOG("[PCM]  target : noise<300  speech>2000  snr>20dB  clip=0");
    RLOG("[PCM]  %s",
        (s_snr_db >= 20 && s_speech_rms >= 2000u && s_clip_cnt == 0u) ? "PASS" :
        (s_speech_rms < 500u)    ? "FAIL: speech too quiet — increase mic_gain or speak closer" :
        (s_noise_rms > 2000u)    ? "FAIL: noise floor too high — hardware/layout issue" :
        (s_clip_cnt > 0u)        ? "FAIL: clipping — reduce mic_gain" :
        (s_snr_db < 10)          ? "FAIL: poor SNR — check PDM filter config" :
                                   "WARN: marginal — may work, tune gain");

    /* ── [3] SAI4 + BDMA registers ──────────────────────────────────────── */
    RLOG("[REG]  SAI4_Block_A->CR1   = 0x%08lX  (mode/clk/mono/firstbit)",  (unsigned long)SAI4_Block_A->CR1);
    RLOG("[REG]  SAI4_Block_A->CR2   = 0x%08lX  (FIFO status/threshold)",   (unsigned long)SAI4_Block_A->CR2);
    RLOG("[REG]  SAI4_Block_A->FRCR  = 0x%08lX  (frame length/sync)",       (unsigned long)SAI4_Block_A->FRCR);
    RLOG("[REG]  SAI4_Block_A->SLOTR = 0x%08lX  (slot active mask)",        (unsigned long)SAI4_Block_A->SLOTR);
    RLOG("[REG]  SAI4_Block_A->SR    = 0x%08lX  (errors: bit1=OVRUDR)",     (unsigned long)SAI4_Block_A->SR);
    RLOG("[REG]  SAI4->PDMCR         = 0x%08lX  (PDM clk enable/mic pairs)",(unsigned long)SAI4->PDMCR);
    RLOG("[REG]  BDMA_Ch1->CCR       = 0x%08lX", (unsigned long)BDMA_Channel1->CCR);
    RLOG("[REG]  BDMA_Ch1->CNDTR     = 0x%08lX  (remaining count)",         (unsigned long)BDMA_Channel1->CNDTR);
    RLOG("[REG]  BDMA_Ch1->CM0AR     = 0x%08lX  (expect 0x38000000)",       (unsigned long)BDMA_Channel1->CM0AR);
    RLOG("--------------------------------------");
}
