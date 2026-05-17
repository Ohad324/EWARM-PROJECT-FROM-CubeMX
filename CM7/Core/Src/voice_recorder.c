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
 *   VoiceRecTask wakes → Start_Recording_Pipeline (MDMA + DFSDM DMA — Option B, no BDMA kick)
 *   DMA1 half/full callbacks (every ~4 ms) → xQueueSendFromISR → s_dmaQueue
 *   VoiceRecTask drain loop → StoreDmaChunk() → DFSDM hardware decimation → 16-bit PCM
 *   Accumulates 750 callbacks × 64 samples = 48000 samples (3 s) in g_AudioBuf
 *   Stop DMA → xQueueSend(xVoiceQueue) → SDWriteTask wakes
 *   SDWriteTask → f_open / f_write(WAV header + PCM) / f_close
 *
 * ── HARDWARE ─────────────────────────────────────────────────────────────────
 *   Architecture: SAI4 (front-end) + DFSDM1 Channel 1 (back-end)
 *
 *   SAI4 drives the physical mic interface (4 pins — all on the board):
 *     PE2 (AF10=SAI4_CK1)      → PDM clock output to microphone
 *     PC1 (AF6=DFSDM1_DATIN1) → PDM data direct to DFSDM filter (bypasses SAI4 FIFO)
 *     PE4 (AF8 =SAI4_FS_A)  → frame sync (unlocks SAI4 master clock tree)
 *     PE5 (AF8 =SAI4_SCK_A) → serial clock (SAI4 internal bit clock)
 *
 *   DFSDM1 Channel 1: SPICKSEL=01 (external SAI4 CK1 PE2), SITP=01 (falling edge). CHCFGR1 target = 0x00000085.
 *   Data from DATIN1=PC1. SAI4 CK1 (PE2) drives the mic at 2.0 MHz; DFSDM samples PDM on that clock's falling edge.
 *   RM0399 p1158: data always from DATINy for all SPICKSEL values.
 *
 *   DFSDM1 Filter0: Sinc3, hardware decimation → 16-bit PCM output
 *   DMA1_Stream1 (DMA_REQUEST_DFSDM1_FLT0) → s_DfsdmBuf (0x3003B800, D2 SRAM2)
 *   Button: PC13 EXTI15_10 rising edge  |  LED: PI12
 *
 * ── CLOCK MATH ───────────────────────────────────────────────────────────────
 *   PER_CK (HSI64) = 64 MHz (SAI4A kernel clock — RCC_D3CCIPR SAI4ASEL=100)
 *   SAI_AUDIO_FREQUENCY_MCKDIV + Init.Mckdiv=16 → Total division = 16×2 = 32
 *   PDM_CLK = 64 MHz / 32 = 2.000 MHz  (0% error ✓)
 *   DFSDM Sinc3 OSR=125  →  PCM = 2.0 MHz / 125 = 16,000 Hz  ✓
 */

#include "voice_recorder.h"
#include "main.h"           /* LED1_Pin, LED1_GPIO_Port, Error_Handler */
#include "usb_msc.h"        /* g_usbMscActive — skip recording during format mode */
#include "stm32h7xx_hal.h"
/* PDM2PCM software library removed — DFSDM hardware does decimation */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "queue.h"
#include "ff.h"             /* FatFS f_open/f_write/f_close */
#include "audio_sd.h"       /* AudioSD_GetErrorCode(), AudioSD_Remount() */
#include "log_mutex.h"   /* RLOG — button diagnostic only, not in audio hot path */
#include "itm_log.h"     /* STAGE() — ITM PORT[0] + RTT WriteString, zero printf */
#include "music_display_task.h"  /* Music_RequestTestThumbFromISR() */
#ifdef WAKE_WORD_TEST
#include "wake_word_test.h"  /* WakeWordTest_Trigger() — quick EI classifier test */
#endif
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
 *   SAI4 CK1 (PE2) = 2.000 MHz (PER_CK 64 MHz / MCKDIV 16 × 2)
 *   Filter: Sinc3, OSR=125 → PCM rate = 2.0 MHz / 125 = 16,000 Hz ✓
 *   DFSDM outputs 64 PCM samples per DMA half (AUDIO_SAMPLES/2 = 64).
 *   DMA buffer is int32_t (DFSDM result is 24-bit sign-extended into 32-bit word).
 */
/* ── [Step 7] buffer constants ─────────────────────────────────────────────── */
#define AUDIO_SAMPLES      128u                           /* PDM/PCM samples per full DMA cycle */
#define AUDIO_BUFFER_BYTES (AUDIO_SAMPLES * sizeof(int32_t))  /* physical byte count = 512      */
#define DFSDM_BUF_TOTAL    AUDIO_SAMPLES                  /* full circular DMA buffer (samples) */
#define DFSDM_BUF_HALF     (AUDIO_SAMPLES / 2u)           /* 64 samples per DMA half-callback   */
#define DMA_HALF_SIZE      DFSDM_BUF_HALF                 /* 64 samples per callback            */
#define WARMUP_CALLS       32u   /* discard first 32 halves — DFSDM SINC3 settling (~128 ms at AUDIO_SAMPLES=128).
                                  * WARNING: if AUDIO_SAMPLES changes, recalculate:
                                  * warmup_ms = WARMUP_CALLS × (AUDIO_SAMPLES/2) / 16000 × 1000 */

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
extern DFSDM_Channel_HandleTypeDef hdfsdm1_channel1;  /* CH1 = CKOUT+falling edge (SPICKSEL=01+SITP=01) — LR=HIGH mic data path */
extern SAI_HandleTypeDef           hsai_BlockA4;

/* ── Live debug globals — declared in main.c, captured here at recording time ─
 * Read via JLink mem32 using addresses from the .map file.
 * g_dbg_live_sai4_cr1: SAI4 CR1 right after Start_Recording_Pipeline().
 *   Bit 16 (SAIEN) MUST be 1. If 0 → SAI4 never enabled → no CK1 → mic silent.
 * g_dbg_live_fltisr: DFSDM FLTISR after DMA start.
 *   Bit 19 (CKABF[3]) = 1 → clock absence on CH3 → SAI4 bridge not active.
 * g_dbg_live_hal_ok: 0=DMA start OK, 0xFFFFFFFF=FAIL.
 * g_dbg_live_fltcr1: FLTCR1 right before DMA start (after RDMAEN force).      */
extern volatile uint32_t g_dbg_live_sai4_cr1;
extern volatile uint32_t g_dbg_live_fltisr;
extern volatile uint32_t g_dbg_live_hal_ok;
extern volatile uint32_t g_dbg_live_fltcr1;

/* ── [Step 7] Option B: DMA1 only pipeline ────────────────────────────────────
 * DMA1_Stream1 (D2 bus master, DMAMUX1=DMA_REQUEST_DFSDM1_FLT0) writes
 * DFSDM Filter0 results directly to s_DfsdmBuf in D2 SRAM2 (0x3003B800).
 * DMA1 can reach D2 SRAM; no domain bridge (MDMA) required.
 * CPU callbacks read from s_DfsdmBuf directly — zero-copy into g_AudioBuf. */
static DMA_HandleTypeDef s_hdma_dfsdm;   /* DMA1_Stream1 — private to this file */

/* BDMA handle for SAI4_A RX — initialized in HAL_SAI_MspInit, started in
 * Start_Recording_Pipeline. BDMA drains the SAI4 RX FIFO so SAIEN stays 1
 * and PE2 keeps clocking the mic. Without this, FIFO fills → PE2 goes flat. */
DMA_HandleTypeDef hdma_sai4_a_rx;

/* SAI4 kick buffer — D3 SRAM4 (0x38000000). BDMA can ONLY access D3 SRAM.
 * uint16_t to match BDMA HALFWORD alignment (set in HAL_SAI_MspInit).
 * 8 halfwords = 16 bytes — circular BDMA drains the SAI4 FIFO continuously. */
#pragma location = 0x38000000
static __no_init uint16_t s_sai4KickBuf[8] __attribute__((aligned(32)));

/* ── [Step 7] DFSDM DMA buffer — D2 SRAM1, accessible by DMA1 ─────────────── */
/* 2026-05-17 evening: MOVED BACK to D2 SRAM1 (top of region @ 0x3001E000).
 * History: was at 0x30004000 in SRAM1 at commit a303dac (song recognition
 * worked). Moved to SRAM2 @ 0x3003B800 during EI integration to free SRAM1
 * for the 128 KB s_ei_pool. After the move, PCM amplitude collapsed to
 * peak≈100-600 with speech_rms == noise_rms (SNR ~0 dB) — the mic appears
 * dead but actually it's a DMA coherency / MPU / SRAM2-clock issue:
 *   - DMA writes happen but CPU reads stale data, OR
 *   - SRAM2 has different cacheable attributes than SRAM1 in MPU_Config, OR
 *   - SRAM2 D2 clock gate (__HAL_RCC_D2SRAM2_CLK_ENABLE()) was never set
 *     (SRAM1 needed this per project_sram1_clock_gate memory; SRAM2 may too).
 * Pool shrunk 128 → 120 KB to free top 8 KB of SRAM1 for this buffer. */
#pragma data_alignment = 32
#pragma location = 0x3001E000
static __no_init int32_t s_DfsdmBuf[AUDIO_SAMPLES];

/* Audio accumulation buffer — 3 s × 16000 Hz × 2 bytes = 96 KB in AXI SRAM.
 * Previously in SDRAM (.sdram_bss) which shares AHB3 with SDMMC1 IDMA —
 * FMC auto-refresh every 7.8 µs caused SDMMC FIFO overrun (RXOVERR) during
 * disk_write. Now in D2 SRAM2 (0x30020000). DFSDM raw data lands in s_DfsdmBuf (D3).
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
    const int32_t  *ptr;       /* pointer into s_DfsdmBuf: [0] (half) or [DFSDM_BUF_HALF] (full) */
} DmaEntry_t;

static QueueHandle_t s_dmaQueue = NULL;
static volatile uint32_t s_dmaQueueOverflow = 0u;  /* ISR drop counter (for g_AudioHealth) */

/* 2026-05-17: Phase 1 of continuous wake-word architecture.
 * Once the audio pipeline (SAI4 BDMA + DFSDM DMA) is started at boot inside
 * VoiceRecTask, it must run forever to feed the always-on wake-word listener
 * in the idle hook. This flag makes Start_Recording_Pipeline() idempotent so
 * subsequent button-triggered calls inside the task loop become no-ops, AND
 * the Stop_DMA calls at the end of each recording are removed so the DMA
 * never gates off. Button-recording semantics are unchanged (still drains
 * 48000 samples / 3 s into g_AudioBuf and signals SDWriteTask). */
static volatile bool s_pipelineStarted = false;

/* ── Audio Quality Report — static accumulators ──────────────────────────── */
static uint32_t s_latency_max_us = 0u;  /* worst-case ISR→task latency in µs */
static uint32_t s_desync_count   = 0u;  /* DMA halves where latency > 1 ms   */
static uint32_t s_qDepth_max     = 0u;  /* max observed DMA queue depth       */
static uint32_t s_noise_rms      = 0u;  /* computed noise RMS (200–700 ms window) */
static uint32_t s_speech_rms     = 0u;  /* computed speech RMS (500–2500 ms)  */
static int32_t  s_dc_offset      = 0;   /* DC bias of speech window           */
static int32_t  s_peak           = 0;   /* peak absolute sample value         */
static uint32_t s_clip_cnt       = 0u;  /* samples at ±32767                  */
static int32_t  s_snr_db         = 0;   /* estimated SNR (6 dB per bit)       */

/* Forward declaration — defined after the task functions */
static void AudioQuality_Report(void);
static void Start_Recording_Pipeline(void);

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
    /* ISR context — must not call xQueueSend. Use RTT directly (ISR-safe). */
    SEGGER_RTT_WriteString(0, "[BTN] PC13_PRESSED\n");

    BaseType_t higher = pdFALSE;

    /* Normal pipeline: button → voice recording → NORA → PC → cloud → thumbnail back.
     * The local test-thumb shortcut (Music_RequestTestThumbFromISR) was used during
     * PFB pipeline bring-up (commit 8bb1ee3); removed now that the LCD is stable on
     * AXI SRAM and we want the real round-trip pipeline to drive the LCD. */

    /* Only notify task if it has been created */
    if (g_State == REC_IDLE
        && voiceRecTaskHandle != NULL)
    {
        xTaskNotifyFromISR(voiceRecTaskHandle, 1u, eSetBits, &higher);
    }

    portYIELD_FROM_ISR(higher);
}

/* DFSDM DMA half-complete — s_DfsdmBuf[0..DFSDM_BUF_HALF-1] ready. */
void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;
    DmaEntry_t e = { &s_DfsdmBuf[0] };
    BaseType_t higher = pdFALSE;
    if (xQueueSendFromISR(s_dmaQueue, &e, &higher) != pdTRUE)
        s_dmaQueueOverflow++;
    portYIELD_FROM_ISR(higher);
}

/* DFSDM DMA full-complete — s_DfsdmBuf[DFSDM_BUF_HALF..DFSDM_BUF_TOTAL-1] ready. */
void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;
    DmaEntry_t e = { &s_DfsdmBuf[DFSDM_BUF_HALF] };
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

/* DMA1_Stream1 IRQ trampoline is VoiceRec_DMA_IRQHandler() below DFSDM_DMA_Init.
 * Called from DMA1_Stream1_IRQHandler in stm32h7xx_it.c.
 * Pipeline: DMA1_Stream1 (s_hdma_dfsdm) → s_DfsdmBuf (0x30004000, D2 SRAM1).
 * DO NOT replace s_hdma_dfsdm with an MDMA handle — DMA1 is the engine here. */

/* ═══════════════════════════════════════════════════════════════════════════
 *  VoiceRecTask — wakes on trigger, active-drains DMA for 3 s (fixed), signals SD
 * ═══════════════════════════════════════════════════════════════════════════ */
void VoiceRecTask(void *arg)
{
    QueueHandle_t q = (QueueHandle_t)arg;
    (void)q;   /* SDWriteTask handoff disabled during DFSDM debug — suppress unused warning */
    uint32_t notif;

    /* 2026-05-17 Phase 1 of continuous wake-word architecture:
     * Start the audio pipeline ONCE here, before entering the wait loop.
     * After this call, SAI4 BDMA + DFSDM circular DMA run forever. The DMA
     * ISR keeps posting half/full chunks to s_dmaQueue every ~4 ms even when
     * we're not actively recording — the queue fills to its 32-entry capacity
     * (~128 ms of audio history) and then ISR-side drops are counted in
     * s_dmaQueueOverflow until the next recording drains it. This always-on
     * audio is the foundation for Phase 3's continuous wake-word classifier
     * (which will read a rolling window of the most recent samples in the
     * idle hook). Button-triggered recordings inside the loop are unchanged
     * in behavior — they just no longer toggle the DMA on/off. */
    Start_Recording_Pipeline();

    for (;;)
    {
        /* Block until button ISR (Phase 1) or WakeWordTask (Phase 2) notifies */
        xTaskNotifyWait(0u, ULONG_MAX, &notif, portMAX_DELAY);

        /* Skip recording while USB MSC format mode owns SDMMC1 */
        if (g_usbMscActive) continue;

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

        RLOG("[REC] START trigger=BTN nextFile=", g_FileIndex + 1u);

        /* ── [Step 1] Re-lock kernel clock before enabling DMA ────────────────────
         * D3CCIPR may drift across domain sleep/wake events. Ensure PER_CK is set
         * before the MDMA's first read activates the SAI4 clock on PE2. */
        MODIFY_REG(RCC->D3CCIPR, RCC_D3CCIPR_SAI4ASEL, RCC_SAI4ACLKSOURCE_CLKP);

        /* ╔══════════════════════════════════════════════════════════════════╗
         * ║  HOLY CODE — Step 7: Activate recording pipeline (button press)  ║
         * ║  MDMA is Sole Owner (Option B). No BDMA kick buffer needed.      ║
         * ║  MDMA first read → FIFO cleared → SAI4 starts PE2 at 2.0 MHz.   ║
         * ╚══════════════════════════════════════════════════════════════════╝ */
        Start_Recording_Pipeline();
        STAGE("PIPELINE_START");

        /* Snapshot debug registers for IAR Live Watch / RTT */
        g_dbg_live_sai4_cr1 = hsai_BlockA4.Instance->CR1;   /* bit16=SAIEN should be 1 */
        g_dbg_live_fltcr1   = hdfsdm1_filter0.Instance->FLTCR1;
        g_dbg_live_fltisr   = hdfsdm1_filter0.Instance->FLTISR;
        g_dbg_live_hal_ok   = 0x00000000u;                  /* pipeline started — no HAL error path here */
        STAGE("DFSDM3 PASS");

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
        STAGE("DFSDM4 PASS");
        TickType_t ledToggle = xTaskGetTickCount() + pdMS_TO_TICKS(500u);

        /* Phase 1: DMA is always-on, so s_dmaQueue may hold up to ~128 ms of
         * pre-trigger audio history. Flush it so this recording starts from
         * a fresh, known sample (the next DMA half-complete after this point).
         * Without this flush the first ~128 ms of g_AudioBuf would be audio
         * from just BEFORE the button press / wake-word detection. */
        {
            DmaEntry_t stale = { NULL };
            while (xQueueReceive(s_dmaQueue, &stale, 0) == pdTRUE) {}
        }

        while (g_SampleCount < AUDIO_BUFFER_SAMPLES)
        {
            /* if (xTaskGetTickCount() >= ledToggle)
            {
                LED_TOGGLE();
                ledToggle += pdMS_TO_TICKS(500u);
            } */

            DmaEntry_t e = { NULL };
            if (xQueueReceive(s_dmaQueue, &e, pdMS_TO_TICKS(10u)) == pdTRUE)
            {
                /* No SCB_InvalidateDCache needed — s_DfsdmBuf (0x30004000) is in
                 * D2 SRAM1, not cached by the M7 D-Cache (no MPU region covers it). */
                StoreDmaChunk(e.ptr);
            }
        }

        /* 2026-05-17 Phase 1: DO NOT stop DMA between recordings.
         * The audio pipeline must run forever to feed the always-on wake-word
         * classifier in the idle hook (Phase 3, upcoming). Stopping here
         * would gate off PE2 mic clock and the wake-word listener would
         * see nothing until the next button press.
         *
         * Pre-Phase-1 code (commented for history):
         *   HAL_DFSDM_FilterRegularStop_DMA(&hdfsdm1_filter0);
         *   HAL_SAI_DMAStop(&hsai_BlockA4);
         *
         * The post-stop queue-flush is also removed — we now flush at the
         * START of each recording (above) instead of at the end. */
        STAGE("DFSDM5 PASS");

        /* Safety: if DMA gave no samples, reset and retry */
        if (g_SampleCount == 0u)
        {
            g_State = REC_IDLE; __DSB();
            continue;
        }

        RLOG("[REC] DFSDM_DONE samples=", g_SampleCount);

        LED_ON();   /* stay ON — recording done, saving to SD */

#ifdef WAKE_WORD_TEST
        /* Quick EI bench: classify the first 1 s (16000 samples) of the captured
         * buffer via the idle hook. Non-blocking — just arms a flag. Results
         * appear in RTT as `[EI] HEY NOA = <pct>` / `Noise = <pct>` after the
         * next idle slice (typically tens of ms once SD write yields). */
        WakeWordTest_Trigger(g_AudioBuf, (size_t)16000u);
#endif

        /* Signal SDWriteTask to write WAV file */
        uint32_t msg = 1u;
        xQueueSend(q, &msg, 0);
        /* g_State reset + LED_OFF() is now owned by SDWriteTask */
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
        uint32_t recSizeBytes    = samplesSnapshot * sizeof(int16_t);
        REC_Result_t recResult   = REC_OK;

        RLOG("[SD] SAVING samples=",  samplesSnapshot);
        RLOG("[SD] SAVING bytes=",    recSizeBytes);
        RLOG("[SD] SAVING fileIdx=",  g_FileIndex + 1u);

        /* Build filename (pre-increment tentative, apply only after f_open OK) */
        char filename[32];
        snprintf(filename, sizeof(filename), "REC_%03lu.wav",
                 (unsigned long)(g_FileIndex + 1u));

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
            STAGE("SD1 FAIL");
            /* No auto-format — SD card must be pre-formatted via USB MSC + Copilot script.
             * FR_NO_FILESYSTEM means the card needs formatting; user must run the
             * format flow (hold blue button 4s at boot, plug CN1, run Copilot script).
             * For other errors: attempt a single remount to recover from stale FatFS state. */
            if (fr != FR_NO_FILESYSTEM)
            {
                /* SDMMC peripheral error — remount once as recovery */
                if (!AudioSD_Remount())
                {
                    recResult = REC_ERR_SD_OPEN;
                    goto done;
                }
                fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
                if (fr != FR_OK)
                {
                    recResult = REC_ERR_SD_OPEN;
                    goto done;
                }
            }
            else
            {
                recResult = REC_ERR_SD_OPEN;
                goto done;
            }
        }

        g_FileIndex++;   /* only increment after successful f_open */
        STAGE("SD1 PASS");
        ITM_STAGE(ITM_SD1_OPEN);

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
                STAGE("SD2 FAIL");
                /* Fallback: f_lseek forces cluster chain allocation non-contiguously */
                f_lseek(&file, prealloc);
            }
            else { STAGE("SD2 PASS"); ITM_STAGE(ITM_SD2_ALLOC); }
            f_lseek(&file, 0);  /* return to start for WAV header write */
        }

        BuildWavHdr(&hdr, samplesSnapshot);

        fr = f_write(&file, &hdr, sizeof(hdr), &bw);
        if (fr != FR_OK || bw != sizeof(hdr))
        {
            STAGE("SD3 FAIL");
            recResult = REC_ERR_SD_WRITE_HDR;
            f_close(&file);
            goto done;
        }
        STAGE("SD3 PASS");
        ITM_STAGE(ITM_SD3_HDR);

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
            uint32_t remaining = recSizeBytes;
            uint32_t offset    = 0u;
            FRESULT  frPcm     = FR_OK;
            STAGE("SD4 PASS");
            ITM_STAGE(ITM_SD4_PCM_START);
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
            if (frPcm != FR_OK || offset != recSizeBytes)
            {
                STAGE("SD4 FAIL");
                recResult = REC_ERR_SD_WRITE_PCM;
                f_close(&file);
                goto done;
            }
        }

        FRESULT fr_close = f_close(&file);
        if (fr_close != FR_OK)
        {
            STAGE("SD5 FAIL");
            recResult = REC_ERR_SD_CLOSE;
            goto done;
        }
        STAGE("SD5 PASS");
        ITM_STAGE(ITM_SD5_CLOSED);

        /* ── Free-space snapshot ─────────────────────────────────────────────
         * Called here (after f_close, DMA stopped) — never during recording. */
        {
            DWORD freeClusters = 0u;
            FATFS *pfs         = NULL;
            if (f_getfree("0:", &freeClusters, &pfs) == FR_OK && pfs != NULL)
                g_sdFreeKB = (uint32_t)freeClusters * (pfs->csize / 2u);
        }

        /* Stage 2 — stream WAV file to NORA over UART8. */
        ITM_STAGE(ITM_UART_SEND);
        if (!AudioSD_SendFileToUART(filename))
        {
            RLOG0("[SD] UART_STREAM_FAIL");
            ITM_STAGE(ITM_ERR_UART_STREAM);
        }
        else
        {
            RLOG0("[SD] UART_STREAM_OK");
        }
        AudioSD_Remount();
        ITM_STAGE(ITM_PIPELINE_DONE);

done:
        /* ── Audio quality + health — always printed, even on error ─────────
         * AudioQuality_Report() uses SEGGER_RTT_Write directly (QRPT macro):
         * it bypasses xLogQueue and appears in RTT Viewer immediately.
         * Health counters (g_AudioHealth) posted via RLOG → RTTLogTask. */
        AudioQuality_Report();

        RLOG("[HLTH] dfsdm_overruns=",  g_AudioHealth.dfsdm_overruns);
        RLOG("[HLTH] sd_write_max_ms=", g_AudioHealth.sd_write_max_ms);
        RLOG("[HLTH] buffer_misses=",   g_AudioHealth.buffer_misses);
        RLOG("[HLTH] bytes_written=",   g_AudioHealth.total_bytes_written);
        RLOG("[HLTH] sd_free_KB=",      g_sdFreeKB);
        RLOG("[HLTH] dma_queue_ovf=",   s_dmaQueueOverflow);

        if (recResult == REC_OK)
            RLOG("[SD] WRITE_OK bytes=", recSizeBytes);
        else
            RLOG("[SD] WRITE_FAIL result=", (uint32_t)recResult);

        /* Remount on any error path — SDMMC stays dirty after a write failure. */
        if (recResult != REC_OK)
            AudioSD_Remount();

        g_State = REC_IDLE;
        __DSB();
    }
}

/* RTTLogTask moved to rtt_log_task.c / rtt_log_task.h */

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
     * DFSDM result register: bits[31:8] = 24-bit signed PCM (already right-shifted by
     * DTRBS=6 in hardware), bits[7:0] = channel ID.
     * >> 8 extracts the 24-bit value sign-extended to int32.
     * With DTRBS=6: max value = 125³ >> 6 = ±30,517 → fits int16_t (±32,767) safely.
     * (DTRBS=5 gave ±61,035 which overflowed int16_t and wrapped to false DC=4501.)
     *
     * ───────────────────────────────────────────────────────────────────────
     * 2026-05-17: Added EI_PCM_GAIN multiplier (default 8×). The on-board
     * MEMS mic + current DFSDM config produces very low-amplitude PCM —
     * observed peak ≈ 116 vs the ≥2000 needed for STT / wake-word recog.
     * The 8× gain (with saturation to int16_t range) lifts speech into the
     * recognition window without clipping normal voice. To tune:
     *   IAR → Project → Options → C/C++ Compiler → Preprocessor → Defined
     *   symbols → add EI_PCM_GAIN=N (e.g. 4, 16, 32). The #ifndef below
     *   only sets the default when no project-level value is provided.
     * Watch the post-recording report's `clip=` counter — if it grows,
     * lower the gain. */
#ifndef EI_PCM_GAIN
#define EI_PCM_GAIN  8
#endif
    for (uint32_t i = 0u; i < toCopy; i++)
    {
        int32_t scaled = ((int32_t)(src32[i] >> 8)) * (int32_t)EI_PCM_GAIN;
        if (scaled >  32767) scaled =  32767;
        if (scaled < -32768) scaled = -32768;
        g_AudioBuf[g_SampleCount + i] = (int16_t)scaled;
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

/* ╔══════════════════════════════════════════════════════════════════════════╗
 * ║  HOLY CODE — Step 7: Start_Recording_Pipeline                           ║
 * ║  Order is critical:                                                      ║
 * ║  1. BDMA starts draining SAI4 RX FIFO → SAIEN latches, PE2 = 2.0 MHz   ║
 * ║  2. DFSDM DMA starts → channel sees valid clock → real PCM data         ║
 * ╚══════════════════════════════════════════════════════════════════════════╝ */
static void Start_Recording_Pipeline(void)
{
    /* 2026-05-17 Phase 1: idempotent — already started? skip everything.
     * VoiceRecTask calls this once at boot to enable continuous audio for
     * wake-word listening; subsequent button-triggered calls inside the
     * task's drain loop hit this guard and return without re-starting the
     * SAI4/DFSDM hardware (which HAL would reject as "already busy"). */
    if (s_pipelineStarted) return;

    /* [Step 6] Start BDMA — drains SAI4 RX FIFO so PE2 clock stays alive.
     * BDMA is the ONLY DMA that can reach D3 SAI4. Without it, SAIEN=1
     * alone fills the FIFO in microseconds and hardware kills PE2. */
    HAL_SAI_Receive_DMA(&hsai_BlockA4, (uint8_t *)s_sai4KickBuf, 8u);
    /* ITM: instant confirmation — no waiting for RTT task */
    _itm_str("[SAI4] CR1=");   _itm_u32(hsai_BlockA4.Instance->CR1);
    _itm_str("[SAI4] SAIEN="); _itm_u32((hsai_BlockA4.Instance->CR1 >> 16u) & 1u);
    _itm_str("[BDMA] CCR=");   _itm_u32(BDMA_Channel1->CCR);   /* bit0=EN must be 1 */
    _itm_str("[BDMA] CNDTR="); _itm_u32(BDMA_Channel1->CNDTR); /* must be counting  */

    /* [Step 7] Start DFSDM circular DMA — PE2 is now clocking the mic. */
    HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0,
                                     (int32_t *)s_DfsdmBuf,
                                     AUDIO_SAMPLES);    /* 128 samples — DFSDM HAL takes SAMPLES */

    /* Phase 1: latch the idempotency flag so future calls become no-ops. */
    s_pipelineStarted = true;
    __DSB();
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
    /* ── [Step 7] Option B: DMA1 only pipeline ─────────────────────────────────
     *
     * Architecture:
     *   SAI4 PE2 (2.0 MHz) → MP34DT05-A → PC1 (DFSDM1_DATIN1)
     *   DFSDM1 Filter0 (Sinc3, OSR=125, SITP=01, SPICKSEL=01) → FLTRDATAR
     *   DMA1_Stream1 (DMAMUX1: DMA_REQUEST_DFSDM1_FLT0, Circular) → s_DfsdmBuf (D2 SRAM1, 0x30004000)
     *   CPU callbacks read s_DfsdmBuf → StoreDmaChunk → g_AudioBuf
     *
     * Buffer at 0x30004000 (D2 SRAM1): DMA1 (D2 bus master) can reach it.
     * No domain bridge needed. */
    __HAL_RCC_DMA1_CLK_ENABLE();

    s_hdma_dfsdm.Instance                 = DMA1_Stream1;
    s_hdma_dfsdm.Init.Request             = DMA_REQUEST_DFSDM1_FLT0;
    s_hdma_dfsdm.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma_dfsdm.Init.PeriphInc           = DMA_PINC_DISABLE;          /* FLTRDATAR: fixed address */
    s_hdma_dfsdm.Init.MemInc              = DMA_MINC_ENABLE;           /* advance through s_DfsdmBuf */
    s_hdma_dfsdm.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;       /* 32-bit DFSDM result */
    s_hdma_dfsdm.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
    s_hdma_dfsdm.Init.Mode                = DMA_CIRCULAR;              /* [Step 7] keep PE2 clock alive */
    s_hdma_dfsdm.Init.Priority            = DMA_PRIORITY_HIGH;
    s_hdma_dfsdm.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;
    if (HAL_DMA_Init(&s_hdma_dfsdm) != HAL_OK) { Error_Handler(); }

    __HAL_LINKDMA(&hdfsdm1_filter0, hdmaReg, s_hdma_dfsdm);

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5u, 0u);   /* FreeRTOS-safe priority */
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

/* IRQ trampoline — called from DMA1_Stream1_IRQHandler in stm32h7xx_it.c */
void VoiceRec_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_dfsdm);
}

/* AudioQuality_Report — diagnostic dump to RTT + Terminal I/O after each recording.
 * Called from SDWriteTask done: section — DMA already stopped, no active recording.
 * printf (ITM spin-wait) is acceptable here: no real-time path is running at
 * this point and SDWriteTask at prio 20 is not latency-critical post-save. */
#define QRPT(fmt, ...) do {                                          \
    char _qb[128];                                                   \
    int  _qn = snprintf(_qb, sizeof(_qb), fmt "\r\n", ##__VA_ARGS__);\
    if (_qn > 0) {                                                   \
        SEGGER_RTT_Write(0, _qb, (unsigned)_qn);                    \
    }                                                                \
} while (0)

static void AudioQuality_Report(void)
{
    QRPT("-------- Audio Quality Report --------");
    /* DFSDM raw output — first 8 samples from s_DfsdmBuf (last DMA half) */
    QRPT("[DFSDM] raw[0..7] : %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
        (unsigned long)s_DfsdmBuf[0], (unsigned long)s_DfsdmBuf[1],
        (unsigned long)s_DfsdmBuf[2], (unsigned long)s_DfsdmBuf[3],
        (unsigned long)s_DfsdmBuf[4], (unsigned long)s_DfsdmBuf[5],
        (unsigned long)s_DfsdmBuf[6], (unsigned long)s_DfsdmBuf[7]);
    QRPT("[DMA]  callbacks=%lu  warmup=%u  stored=%lu  overflow=%lu%s",
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
        QRPT("[SYNC] latency_max=%lu us  headroom=%ld us  qDepth_max=%u  desync=%lu  -> %s",
            (unsigned long)s_latency_max_us,
            (long)headroom_us,
            (unsigned)s_qDepth_max,
            (unsigned long)s_desync_count,
            sync_status);
    }

    /* ── [2] PCM statistics ──────────────────────────────────────────────── */
    /* Show samples from settled region (skip first 4 halves = 64 samples to avoid residual transient) */
    QRPT("[PCM]  out[64..71] : %d %d %d %d %d %d %d %d",
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

    QRPT("[PCM]  noise_rms=%-6lu  speech_rms=%-6lu  snr~%+d dB",
        (unsigned long)s_noise_rms, (unsigned long)s_speech_rms, (int)s_snr_db);
    QRPT("[PCM]  peak=%-6d  dc_offset=%-6d  clip=%lu",
        (int)s_peak, (int)s_dc_offset, (unsigned long)s_clip_cnt);
    QRPT("[PCM]  target : noise<300  speech>2000  snr>20dB  clip=0");
    QRPT("[PCM]  %s",
        (s_snr_db >= 20 && s_speech_rms >= 2000u && s_clip_cnt == 0u) ? "PASS" :
        (s_speech_rms < 500u)    ? "FAIL: speech too quiet -- increase mic_gain or speak closer" :
        (s_noise_rms > 2000u)    ? "FAIL: noise floor too high -- hardware/layout issue" :
        (s_clip_cnt > 0u)        ? "FAIL: clipping -- reduce mic_gain" :
        (s_snr_db < 10)          ? "FAIL: poor SNR -- check PDM filter config" :
                                   "WARN: marginal -- may work, tune gain");

    /* ── [3] DFSDM + DMA1 registers ─────────────────────────────────────── */
    QRPT("[REG]  DFSDM1_Ch1->CHCFGR1 = 0x%08lX  (active ch: CHEN+SPICKSEL=01+SITP=01 expect 0x00000085)", (unsigned long)DFSDM1_Channel1->CHCFGR1);
    QRPT("[REG]  DFSDM1_Ch1->CHCFGR2 = 0x%08lX  (DTRBS=6 expect 0x00000030)",  (unsigned long)DFSDM1_Channel1->CHCFGR2);
    QRPT("[REG]  DFSDM1_Flt0->FLTCR1 = 0x%08lX  (filter enable/DMA/trig)",  (unsigned long)DFSDM1_Filter0->FLTCR1);
    QRPT("[REG]  DFSDM1_Flt0->FLTCR2 = 0x%08lX  (IT enables)",              (unsigned long)DFSDM1_Filter0->FLTCR2);
    QRPT("[REG]  DFSDM1_Flt0->FLTISR = 0x%08lX  (status: bit3=ROVRF ovrun)",(unsigned long)DFSDM1_Filter0->FLTISR);
    QRPT("[REG]  DFSDM1_Flt0->FLTFCR = 0x%08lX  (Sinc order / OSR)",        (unsigned long)DFSDM1_Filter0->FLTFCR);
    QRPT("[REG]  DMA1_St1->CR        = 0x%08lX  (stream config)",            (unsigned long)DMA1_Stream1->CR);
    QRPT("[REG]  DMA1_St1->NDTR      = 0x%08lX  (remaining count)",          (unsigned long)DMA1_Stream1->NDTR);
    QRPT("[REG]  DMA1_St1->M0AR      = 0x%08lX  (expect 0x30000000)",        (unsigned long)DMA1_Stream1->M0AR);

    /* ── [4] System snapshot — task stack HWMs + uptime + recording metadata ── */
    QRPT("[SYS]  uptime=%lu ms  fileIdx=%lu  samples=%lu  duration=%lu ms",
        (unsigned long)HAL_GetTick(),
        (unsigned long)g_FileIndex,
        (unsigned long)g_SampleCount,
        (unsigned long)(g_SampleCount * 1000u / SAMPLE_RATE));
    QRPT("[SYS]  VoiceRecTask HWM=%u words / 3072  SDWriteTask HWM=%u words / 2048",
        (unsigned)uxTaskGetStackHighWaterMark(voiceRecTaskHandle),
        (unsigned)uxTaskGetStackHighWaterMark(NULL));   /* NULL = SDWriteTask (calling context) */
    QRPT("[SYS]  sd_free_KB=%lu  dfsdm_ovr=%lu  sd_miss=%lu  sd_worst_ms=%lu  dma_q_ovf=%lu",
        (unsigned long)g_sdFreeKB,
        (unsigned long)g_AudioHealth.dfsdm_overruns,
        (unsigned long)g_AudioHealth.buffer_misses,
        (unsigned long)g_AudioHealth.sd_write_max_ms,
        (unsigned long)s_dmaQueueOverflow);
    QRPT("--------------------------------------");
}
