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
 *   SDWriteTask → f_open / f_write(WAV header + PCM) / f_close
 *
 * ── HARDWARE ─────────────────────────────────────────────────────────────────
 *   Architecture: SAI4 (front-end) + DFSDM1 Channel 3 (back-end)
 *
 *   SAI4 drives the physical mic interface (4 pins — all on the board):
 *     PE2 (AF10=SAI4_CK1)   → PDM clock output to microphone
 *     PC1 (AF10=SAI4_D1)    → PDM data input from microphone
 *     PE4 (AF8 =SAI4_FS_A)  → frame sync (unlocks SAI4 master clock tree)
 *     PE5 (AF8 =SAI4_SCK_A) → serial clock (SAI4 internal bit clock)
 *
 *   DFSDM1 Channel 3 receives PDM from SAI4 via the internal silicon bridge
 *   (SPICKSEL=11 in CHCFGR1 — set by direct register write after ChannelInit).
 *   No external DFSDM pins used: PD3/PC7 configured by CubeMX MSP but unused.
 *
 *   DFSDM1 Filter0: Sinc3, hardware decimation → 16-bit PCM output
 *   DMA1_Stream1 → g_DfsdmBuf in D2 SRAM (0x30000000)
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
/* PDM2PCM software library removed — DFSDM hardware does decimation */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "ff.h"             /* FatFS f_open/f_write/f_close */
#include "audio_sd.h"       /* AudioSD_GetErrorCode(), AudioSD_Remount() */
/* log_mutex.h removed — no RTT logging in hot path */
#include <string.h>
#include <stdio.h>       /* snprintf */
#include <limits.h>      /* ULONG_MAX */

/* ── Audio constants ─────────────────────────────────────────────────────── */
#define SAMPLE_RATE           16000u
#define RECORD_DURATION_S     3u
#define AUDIO_BUFFER_SAMPLES  (SAMPLE_RATE * RECORD_DURATION_S)  /* 48000 */
#define RECORD_MS             (RECORD_DURATION_S * 1000u)
#define DEBOUNCE_MS           300u

/* DFSDM buffer sizing:
 *   APB2 = 100 MHz, CKOUTDIV=24 → CKOUT = 100 MHz / (2×25) = 2.0 MHz to mic (MP34DT05-A spec: 1.2–3.25 MHz ✓)
 *   Filter: Sinc3, OSR=125 → PCM rate = 2.0 MHz / 125 = 16,000 Hz ✓
 *   DFSDM outputs 16 PCM samples per DMA half (same rate as before).
 *   DMA buffer is int32_t (DFSDM result is 24-bit sign-extended into 32-bit word).
 */
#define DFSDM_BUF_HALF   16u   /* PCM samples per DMA half  */
#define DFSDM_BUF_TOTAL  32u   /* full circular DMA buffer  */
#define DMA_HALF_SIZE    DFSDM_BUF_HALF   /* 16 PCM samples per callback */
#define WARMUP_CALLS     32u   /* discard first 32 halves (~2 ms) — DFSDM SINC3 settling */

/* ── WAV header — 512 bytes, sector-aligned (little-endian, packed) ─────────
 * Standard 44-byte WAV header leaves PCM data at offset 44 — not sector-aligned.
 * On exFAT + SDMMC 4-bit mode, writes not aligned to 512-byte sector boundaries
 * trigger a Read-Modify-Write cycle (~30% extra latency per write).
 *
 * Fix: insert a JUNK chunk (460 bytes of zeros) between fmt and data chunks.
 * This pads the total header to exactly 512 bytes so PCM data starts at
 * offset 512 — the first byte of sector 1. All subsequent 4KB writes land on
 * exact sector boundaries. No RMW, no latency penalty.
 *
 * Layout:
 *   [  0.. 11]  RIFF header        12 bytes
 *   [ 12.. 35]  fmt  chunk         24 bytes
 *   [ 36..503]  JUNK chunk        468 bytes  ("JUNK" + 4-byte size + 460 zeros)
 *   [504..511]  data chunk header   8 bytes
 *   [512..   ]  PCM samples        sector-aligned ✓
 */
#pragma pack(push, 1)
typedef struct {
    /* RIFF header */
    char     riff[4];           /* "RIFF"                              */
    uint32_t chunkSize;         /* file_size - 8  = 504 + PCM_bytes   */
    char     wave[4];           /* "WAVE"                              */
    /* fmt chunk */
    char     fmt[4];            /* "fmt "                              */
    uint32_t subchunk1Size;     /* 16 for PCM                          */
    uint16_t audioFormat;       /* 1 = PCM                             */
    uint16_t numChannels;       /* 1 = mono                            */
    uint32_t sampleRate;        /* 16000                               */
    uint32_t byteRate;          /* 32000                               */
    uint16_t blockAlign;        /* 2                                   */
    uint16_t bitsPerSample;     /* 16                                  */
    /* JUNK chunk — 468 bytes, pads header to 512 */
    char     junk[4];           /* "JUNK"                              */
    uint32_t junkSize;          /* 460                                 */
    uint8_t  junkData[460];     /* zeros                               */
    /* data chunk header */
    char     data[4];           /* "data"                              */
    uint32_t subchunk2Size;     /* PCM data byte count                 */
} WavHdr_t;
#pragma pack(pop)

/* Compile-time check: header must be exactly 512 bytes */
typedef char _WavHdr512Check[(sizeof(WavHdr_t) == 512u) ? 1 : -1];

/* ── Peripheral handles — owned by CubeMX (main.c), used here via extern ── */
extern DFSDM_Filter_HandleTypeDef  hdfsdm1_filter0;
extern DFSDM_Channel_HandleTypeDef hdfsdm1_channel0;  /* CH0 = SAI4 MicPair1 rising edge test */
extern SAI_HandleTypeDef           hsai_BlockA4;

/* ── Live debug globals — declared in main.c, captured here at recording time ─
 * Read via JLink mem32 using addresses from the .map file.
 * g_dbg_live_sai4_cr1: SAI4 CR1 right after __HAL_SAI_ENABLE.
 *   Bit 16 (SAIEN) MUST be 1. If 0 → SAI4 never enabled → no CK1 → mic silent.
 * g_dbg_live_fltisr: DFSDM FLTISR after DMA start.
 *   Bit 19 (CKABF[3]) = 1 → clock absence on CH3 → SAI4 bridge not active.
 * g_dbg_live_hal_ok: 0=DMA start OK, 0xFFFFFFFF=FAIL.
 * g_dbg_live_fltcr1: FLTCR1 right before DMA start (after RDMAEN force).      */
extern volatile uint32_t g_dbg_live_sai4_cr1;
extern volatile uint32_t g_dbg_live_fltisr;
extern volatile uint32_t g_dbg_live_hal_ok;
extern volatile uint32_t g_dbg_live_fltcr1;

/* DMA handle — set up in DFSDM_DMA_Init(), linked to hdfsdm1_filter0 */
static DMA_HandleTypeDef           s_hdma_dfsdm;

/* ── DFSDM DMA buffer — int32_t (DFSDM result is 24-bit sign-extended to 32 bits).
 * Placed in D2 SRAM1 (0x30000000, 128 KB) — DMA1 can access this region AND
 * it is outside the M7 D-Cache address space: SCB_InvalidateDCache is NOT needed.
 * This also avoids AXI bus contention with SDMMC1 IDMA (which lives on D1 AXI).
 * 32-byte aligned for future-proofing (consistent with other DMA buffers). */
#pragma location = 0x30000000
static __no_init int32_t g_DfsdmBuf[DFSDM_BUF_TOTAL]  __attribute__((aligned(32)));

/* Audio accumulation buffer — 3 s × 16000 Hz × 2 bytes = 96 KB in AXI SRAM.
 * Previously in SDRAM (.sdram_bss) which shares AHB3 with SDMMC1 IDMA —
 * FMC auto-refresh every 7.8 µs caused SDMMC FIFO overrun (RXOVERR) during
 * disk_write. Now moved to D2 SRAM2 (0x30020000) — same domain as g_DfsdmBuf.
 * StoreDmaChunk copies D2→D2 without crossing the AXI bus matrix.
 * SDMMC IDMA reads via the s_pcmBounce AXI SRAM bounce buffer (unchanged). */
#pragma location = 0x30020000
__no_init int16_t g_AudioBuf[AUDIO_BUFFER_SAMPLES] __attribute__((aligned(32)));

/* No staging buffer needed — DFSDM hardware does PDM→PCM decimation. */

/* ── Audio health monitor ────────────────────────────────────────────────── */
volatile AudioHealth_t g_AudioHealth = {0};

/* ── Recording state ─────────────────────────────────────────────────────── */
volatile RecState_t  g_State       = REC_IDLE;
volatile uint32_t    g_SampleCount = 0u;
volatile uint32_t    g_sdFreeKB    = 0u;  /* updated by SDWriteTask after each f_close */
static   uint32_t    g_DmaCallCount = 0u;  /* total DMA halves consumed — used for ping-pong halfIdx */
static   uint32_t    g_FileIndex   = 0u;

/* ── FreeRTOS objects ────────────────────────────────────────────────────── */
TaskHandle_t       voiceRecTaskHandle = NULL;

typedef struct {
    const int32_t  *ptr;       /* buffer pointer (&g_DfsdmBuf[0] or [DFSDM_BUF_HALF]) */
} DmaEntry_t;

static QueueHandle_t s_dmaQueue = NULL;
static volatile uint32_t s_dmaQueueOverflow = 0u;  /* ISR drop counter (for g_AudioHealth) */

/* ── LED helpers ─────────────────────────────────────────────────────────── */
#define LED_ON()     HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET)
#define LED_OFF()    HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET)
#define LED_TOGGLE() HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin)

/* ── Forward declarations ────────────────────────────────────────────────── */
static void Button_GPIO_Init(void);
static void DFSDM_DMA_Init(void);
static void StoreDmaChunk(const int32_t *src32);
static void BuildWavHdr(WavHdr_t *h, uint32_t nSamples);

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
    configASSERT(s_dmaQueue);

    Button_GPIO_Init();
    DFSDM_DMA_Init();   /* DMA only — CubeMX owns channel/filter/GPIO init */

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

/* DFSDM DMA half-complete — g_DfsdmBuf[0..DFSDM_BUF_HALF-1] ready. */
void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;
    DmaEntry_t e = { &g_DfsdmBuf[0] };
    BaseType_t higher = pdFALSE;
    if (xQueueSendFromISR(s_dmaQueue, &e, &higher) != pdTRUE)
        s_dmaQueueOverflow++;
    portYIELD_FROM_ISR(higher);
}

/* DFSDM DMA full-complete — g_DfsdmBuf[DFSDM_BUF_HALF..DFSDM_BUF_TOTAL-1] ready. */
void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;
    DmaEntry_t e = { &g_DfsdmBuf[DFSDM_BUF_HALF] };
    BaseType_t higher = pdFALSE;
    if (xQueueSendFromISR(s_dmaQueue, &e, &higher) != pdTRUE)
        s_dmaQueueOverflow++;
    portYIELD_FROM_ISR(higher);
}

/* DFSDM hardware overrun — RDATFIFO not read before next result was available.
 * This means DMA1 is lagging and audio data was silently discarded by hardware.
 * Any non-zero value here means the recording has gaps. */
void HAL_DFSDM_FilterRegConvErrorCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    (void)hdfsdm;
    g_AudioHealth.dfsdm_overruns++;
}

/* IRQ trampoline — called from DMA1_Stream1_IRQHandler in stm32h7xx_it.c */
void VoiceRec_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_dfsdm);
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
        g_State              = REC_RECORDING;
        /* Reset health counters for this recording */
        g_AudioHealth.dfsdm_overruns      = 0u;
        g_AudioHealth.sd_write_max_ms     = 0u;
        g_AudioHealth.buffer_misses       = 0u;
        g_AudioHealth.total_bytes_written = 0u;
        __DSB();

        /* ── Enable SAI4 — drives the PDM clock to the microphone via PE2/SAI4_CK1.
         * In the DFSDM bridge path SAI4 provides clock only; no SAI4 DMA is used.
         * (hsai_BlockA4.hdmarx is NULL — BDMA is not wired for this path.)
         * DFSDM1_Channel3 receives the PDM bitstream via internal silicon routing.
         * Startup order: SAI4 clock running → DFSDM DMA started. */
        __HAL_SAI_ENABLE(&hsai_BlockA4);
        g_dbg_live_sai4_cr1 = hsai_BlockA4.Instance->CR1;

        /* HAL_DFSDM_FilterInit sets RDMAEN before DFEN — on STM32H7 the subsequent
         * HAL_DFSDM_FilterConfigRegChannel RMW on FLTCR1 (with DFEN=1) can silently
         * clear RDMAEN. HAL_DFSDM_FilterRegularStart_DMA checks RDMAEN and returns
         * HAL_ERROR if it is 0, so the DMA never starts.
         * Fix: force RDMAEN=1 here, after all init calls are complete and just before
         * starting the DMA. This is safe — RDMAEN is writable when DFEN=1. */
        hdfsdm1_filter0.Instance->FLTCR1 |= DFSDM_FLTCR1_RDMAEN;
        g_dbg_live_fltcr1 = hdfsdm1_filter0.Instance->FLTCR1;

        /* Start DFSDM DMA (DMA1_Stream1 → g_DfsdmBuf in D2 SRAM) */
        HAL_StatusTypeDef hal =
            HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0,
                                             g_DfsdmBuf,
                                             DFSDM_BUF_TOTAL);
        g_dbg_live_hal_ok = (hal == HAL_OK) ? 0x00000000u : 0xFFFFFFFFu;
        g_dbg_live_fltisr = hdfsdm1_filter0.Instance->FLTISR;
        if (hal != HAL_OK)
        {
            HAL_SAI_DMAStop(&hsai_BlockA4);
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
                /* D-Cache coherency — g_DfsdmBuf is at 0x30000000 (D2 SRAM).
                 * D2 SRAM is cacheable under the default Cortex-M7 memory map
                 * (only one MPU region is configured: Flash at 0x08000000).
                 * DMA1 writes to physical SRAM, bypassing cache. Without this
                 * invalidation the CPU reads stale cache lines and converts the
                 * same value to PCM 3000 times — producing constant-DC output.
                 * Half size = DFSDM_BUF_HALF × 4 = 64 bytes = 2 cache lines. */
                SCB_InvalidateDCache_by_Addr((uint32_t*)e.ptr,
                                             DFSDM_BUF_HALF * sizeof(int32_t));
                StoreDmaChunk(e.ptr);
            }
        }

        /* Stop order: DFSDM first (consumer), then SAI4 (clock source) */
        HAL_DFSDM_FilterRegularStop_DMA(&hdfsdm1_filter0);
        HAL_SAI_DMAStop(&hsai_BlockA4);

        /* Drain any stale queue entries from callbacks that fired after stop */
        {
            DmaEntry_t discard = { NULL, 0u };
            while (xQueueReceive(s_dmaQueue, &discard, 0) == pdTRUE) {}
        }

        /* Safety: if DMA gave no samples, reset and retry */
        if (g_SampleCount == 0u)
        {
            g_State = REC_IDLE; __DSB();
            continue;
        }

        LED_ON();   /* stay ON — recording done, saving to SD */

        /* Signal SDWriteTask — disabled while debugging DFSDM pipeline */
        /* uint32_t msg = 1u; */
        /* xQueueSend(q, &msg, 0); */
        g_State = REC_IDLE; __DSB();  /* must reset — normally SDWriteTask does this */
        LED_OFF();
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
            if (fr == FR_NO_FILESYSTEM)
            {
                if (!AudioSD_Format())
                {
                    logMsg.result = REC_ERR_SD_OPEN;
                    goto done;
                }
            }
            else
            {
                /* SDMMC peripheral error — remount once as recovery */
                if (!AudioSD_Remount())
                {
                    logMsg.result = REC_ERR_SD_OPEN;
                    goto done;
                }
            }
            fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
            if (fr != FR_OK)
            {
                logMsg.result = REC_ERR_SD_OPEN;
                goto done;
            }
        }

        g_FileIndex++;   /* only increment after successful f_open */

        /* Pre-allocate contiguous clusters before writing.
         * Without this, every f_write triggers a FAT cluster search (tens of ms on exFAT)
         * causing DMA queue overflow → audio gaps → beeps.
         *
         * Strategy (per user architecture recommendation):
         *   1. f_expand(opt=1): allocates a physically contiguous block — fastest SD DMA.
         *      FF_USE_EXPAND=1 enabled in ffconf.h.
         *   2. Fallback to f_lseek if f_expand fails (fragmented card, FATFS version).
         *   3. f_lseek(0) to return file pointer to start before writing header. */
        {
            FSIZE_t prealloc = (FSIZE_t)(sizeof(WavHdr_t) + samplesSnapshot * sizeof(int16_t));
            FRESULT fr_exp = f_expand(&file, prealloc, 1);  /* opt=1: allocate contiguous */
            if (fr_exp != FR_OK)
            {
                /* Fallback: f_lseek forces cluster chain allocation non-contiguously */
                f_lseek(&file, prealloc);
            }
            f_lseek(&file, 0);  /* return to start for WAV header write */
        }

        BuildWavHdr(&hdr, samplesSnapshot);

        fr = f_write(&file, &hdr, sizeof(hdr), &bw);
        if (fr != FR_OK || bw != sizeof(hdr))
        {
            logMsg.result = REC_ERR_SD_WRITE_HDR;
            f_close(&file);
            goto done;
        }

        /* Write PCM via AXI SRAM bounce buffer.
         * g_AudioBuf is in D2 SRAM2 (0x30020000). SDMMC IDMA accesses memory via
         * AXI bus matrix. D2 SRAM is not guaranteed reachable by SDMMC IDMA in all
         * STM32H7 silicon revisions — use a static AXI SRAM bounce buffer to be safe.
         *
         * AXI SRAM (0x24000000) is cached (M7 D-Cache). Before handing a chunk to
         * SDMMC IDMA we must flush the D-Cache for that buffer, otherwise SDMMC
         * reads stale data from physical RAM while the real data sits in cache lines.
         * SCB_CleanDCache_by_Addr() writes dirty lines back to physical RAM.
         *
         * Latency measurement: HAL_GetTick() before/after each f_write.
         * g_AudioHealth.sd_write_max_ms updated if this write is the slowest seen.
         * 4KB at 16kHz×2byte = 128ms window — if max_ms > 100 we risk queue overflow. */
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

                /* Flush D-Cache: ensure SDMMC IDMA reads the freshly copied bytes,
                 * not stale lines. Size must be rounded up to 32-byte cache line. */
                SCB_CleanDCache_by_Addr((uint32_t *)s_pcmBounce,
                                        (int32_t)((chunk + 31u) & ~31u));

                uint32_t t0 = HAL_GetTick();
                frPcm = f_write(&file, s_pcmBounce, chunk, &bw);
                uint32_t dur = HAL_GetTick() - t0;

                if (dur > g_AudioHealth.sd_write_max_ms)
                    g_AudioHealth.sd_write_max_ms = dur;

                if (frPcm != FR_OK || bw != chunk)
                {
                    g_AudioHealth.buffer_misses++;
                    frPcm = FR_DISK_ERR; break;
                }
                g_AudioHealth.total_bytes_written += chunk;
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

        /* ── Free-space snapshot ─────────────────────────────────────────────
         * Called here (after f_close, DMA stopped) — never during recording.
         * Principle 1: Zero Mutex Contention  — SD already mounted, no contest
         * Principle 2: Zero Resource Theft     — no background task polls SD
         * Principle 3: Predictability          — only fires post-write, not mid-stream */
        {
            DWORD freeClusters = 0u;
            FATFS *pfs         = NULL;
            if (f_getfree("0:", &freeClusters, &pfs) == FR_OK && pfs != NULL)
                g_sdFreeKB = (uint32_t)freeClusters * (pfs->csize / 2u);
        }

        /* Stage 2 (UART streaming to NORA) removed — not needed for standalone recording.
         * WAV file stays on SD card, accessible via USB MSC or card reader. */

done:
        /* Remount on any error path — SDMMC stays dirty after a write failure
         * and all subsequent FatFS calls return FR_NOT_READY until the peripheral
         * is reset. */
        if (logMsg.result != REC_OK)
            AudioSD_Remount();

        g_State = REC_IDLE;
        __DSB();
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  RTTLogTask — polls SD_DETECT, drives LED2
 * ═══════════════════════════════════════════════════════════════════════════ */
void RTTLogTask(void *arg)
{
    (void)arg;
    for (;;)
    {
        /* Poll SD_DETECT (PI8, active-low) every 200 ms.
         * LED2 (PI13) ON = card present, OFF = card absent. */
        GPIO_PinState det = HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_8);
        HAL_GPIO_WritePin(GPIOI, GPIO_PIN_13,
                          (det == GPIO_PIN_RESET) ? GPIO_PIN_SET : GPIO_PIN_RESET);
        vTaskDelay(pdMS_TO_TICKS(200u));
    }
}

/* HealthMonTask removed — health reporting moved into SDWriteTask.
 * SDWriteTask logs [HEALTH] lines after every successful f_close():
 *   dfsdm_overruns, sd_write_max_ms, buffer_misses, bytes_written,
 *   sd_free (KB), sd_err, and STATUS (OK / WARNING / FAIL).
 * No periodic background task needed — the data is naturally available
 * at the only moment that matters: right after each recording completes. */

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
static void StoreDmaChunk(const int32_t *src32)
{
    if (g_SampleCount >= AUDIO_BUFFER_SAMPLES) return;

    g_DmaCallCount++;

    /* Skip first WARMUP_CALLS halves — DFSDM SINC3 filter settling transient */
    if (g_DmaCallCount <= WARMUP_CALLS) return;

    uint32_t remaining = AUDIO_BUFFER_SAMPLES - g_SampleCount;
    uint32_t toCopy    = (remaining < DMA_HALF_SIZE) ? remaining : DMA_HALF_SIZE;

    /* Convert DFSDM 32-bit output → 16-bit PCM.
     * DFSDM result register: 24-bit signed value in bits[31:8], channel in bits[7:0].
     * RightBitShift=8 applied by hardware shifts the result right 8 bits inside the
     * peripheral, so the meaningful audio sits in bits[15:0] after one more >>8 here.
     * Reference: audio_rec.c SendPCMFrame() pattern — same shift confirmed working. */
    for (uint32_t i = 0u; i < toCopy; i++)
    {
        g_AudioBuf[g_SampleCount + i] = (int16_t)(src32[i] >> 8);
    }
    g_SampleCount += toCopy;
}

static void BuildWavHdr(WavHdr_t *h, uint32_t nSamples)
{
    uint32_t dataBytes = nSamples * sizeof(int16_t);
    /* RIFF — chunkSize = file_size - 8.
     * file_size = 512 (this header) + dataBytes
     * chunkSize = 512 + dataBytes - 8 = 504 + dataBytes */
    h->riff[0]='R'; h->riff[1]='I'; h->riff[2]='F'; h->riff[3]='F';
    h->chunkSize      = 504u + dataBytes;
    h->wave[0]='W'; h->wave[1]='A'; h->wave[2]='V'; h->wave[3]='E';
    /* fmt chunk */
    h->fmt[0]='f';  h->fmt[1]='m';  h->fmt[2]='t';  h->fmt[3]=' ';
    h->subchunk1Size  = 16u;
    h->audioFormat    = 1u;
    h->numChannels    = 1u;
    h->sampleRate     = SAMPLE_RATE;
    h->byteRate       = SAMPLE_RATE * 2u;
    h->blockAlign     = 2u;
    h->bitsPerSample  = 16u;
    /* JUNK chunk — 460 bytes of zeros, total chunk = 468 bytes */
    h->junk[0]='J'; h->junk[1]='U'; h->junk[2]='N'; h->junk[3]='K';
    h->junkSize       = 460u;
    memset(h->junkData, 0, sizeof(h->junkData));
    /* data chunk header — PCM starts at offset 512 (sector boundary) */
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

static void DFSDM_DMA_Init(void)
{
    /* ── DMA1 Stream1 for DFSDM1 Filter0 ─────────────────────────────────
     *
     * CubeMX owns: DFSDM1 channel/filter init (MX_DFSDM1_Init),
     *              GPIO + clocks (HAL_DFSDM_FilterMspInit / ChannelMspInit),
     *              SAI4 front-end (MX_SAI4_Init).
     *
     * CubeMX does NOT generate DMA for DFSDM (IOC gap) — we do it here.
     *
     * Architecture:
     *   SAI4 (PE2/PC1/PE4/PE5) → physical mic → internal silicon routing
     *   DFSDM1 Channel 3 (SPICKSEL=11: SAI4 Block A bridge) → Sinc3 decimation → PCM
     *   DMA1 Stream1 → g_DfsdmBuf in D2 SRAM (0x30000000)
     *
     * DMA1/DMA2 can access D2 SRAM where g_DfsdmBuf lives.
     * 32-bit word: DFSDM result register is 32 bits.
     * Circular mode: continuous ping-pong for the drain loop. */
    __HAL_RCC_DMA1_CLK_ENABLE();

    s_hdma_dfsdm.Instance                 = DMA1_Stream1;
    s_hdma_dfsdm.Init.Request             = DMA_REQUEST_DFSDM1_FLT0;
    s_hdma_dfsdm.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma_dfsdm.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_hdma_dfsdm.Init.MemInc              = DMA_MINC_ENABLE;
    s_hdma_dfsdm.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
    s_hdma_dfsdm.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    s_hdma_dfsdm.Init.Mode                = DMA_CIRCULAR;
    s_hdma_dfsdm.Init.Priority            = DMA_PRIORITY_VERY_HIGH;
    s_hdma_dfsdm.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&s_hdma_dfsdm) != HAL_OK) { Error_Handler(); }

    __HAL_LINKDMA(&hdfsdm1_filter0, hdmaReg, s_hdma_dfsdm);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5u, 0u); /* Must be >= configMAX_SYSCALL_INTERRUPT_PRIORITY (5) for xQueueSendFromISR */
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

/* AudioQuality_Report removed — all RLOG removed per project policy.
 * Raw data still accessible via IAR Watch: g_AudioBuf, g_DfsdmBuf, g_AudioHealth. */
#if 0  /* kept for reference, never compiled */
static void AudioQuality_Report(void)
{
    RLOG("-------- Audio Quality Report --------");
    /* DFSDM raw output — first 8 samples from last DMA half */
    RLOG("[DFSDM] raw[0..7] : %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
        (unsigned long)g_DfsdmBuf[0], (unsigned long)g_DfsdmBuf[1],
        (unsigned long)g_DfsdmBuf[2], (unsigned long)g_DfsdmBuf[3],
        (unsigned long)g_DfsdmBuf[4], (unsigned long)g_DfsdmBuf[5],
        (unsigned long)g_DfsdmBuf[6], (unsigned long)g_DfsdmBuf[7]);
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

    /* ── [3] DFSDM + DMA1 registers ─────────────────────────────────────── */
    RLOG("[REG]  DFSDM1_Ch0->CHCFGR1 = 0x%08lX  (global: DFSDMEN+CKOUTDIV expect 0x80180000)", (unsigned long)DFSDM1_Channel0->CHCFGR1);
    RLOG("[REG]  DFSDM1_Ch0->CHCFGR1 = 0x%08lX  (active ch: SPICKSEL=11 expect 0x0000008C)", (unsigned long)DFSDM1_Channel0->CHCFGR1);
    RLOG("[REG]  DFSDM1_Ch0->CHCFGR2 = 0x%08lX  (offset/shift)",            (unsigned long)DFSDM1_Channel0->CHCFGR2);
    RLOG("[REG]  DFSDM1_Flt0->FLTCR1 = 0x%08lX  (filter enable/DMA/trig)",  (unsigned long)DFSDM1_Filter0->FLTCR1);
    RLOG("[REG]  DFSDM1_Flt0->FLTCR2 = 0x%08lX  (IT enables)",              (unsigned long)DFSDM1_Filter0->FLTCR2);
    RLOG("[REG]  DFSDM1_Flt0->FLTISR = 0x%08lX  (status: bit3=ROVRF ovrun)",(unsigned long)DFSDM1_Filter0->FLTISR);
    RLOG("[REG]  DFSDM1_Flt0->FLTFCR = 0x%08lX  (Sinc order / OSR)",        (unsigned long)DFSDM1_Filter0->FLTFCR);
    RLOG("[REG]  DMA1_St1->CR        = 0x%08lX  (stream config)",            (unsigned long)DMA1_Stream1->CR);
    RLOG("[REG]  DMA1_St1->NDTR      = 0x%08lX  (remaining count)",          (unsigned long)DMA1_Stream1->NDTR);
    RLOG("[REG]  DMA1_St1->M0AR      = 0x%08lX  (expect 0x30000000)",        (unsigned long)DMA1_Stream1->M0AR);
    RLOG("--------------------------------------");
}
#endif /* 0 */
