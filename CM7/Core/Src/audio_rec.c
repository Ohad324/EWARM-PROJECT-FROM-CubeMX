/*
 * audio_rec.c — Button-triggered MEMS audio recording
 *
 * HOW IT WORKS:
 *   1. Press blue button (PC13)  → DFSDM starts converting mic PDM signal to PCM via DMA
 *   2. DMA fills a double-buffer → every 512 samples, a callback fires and sends a frame over UART8
 *   3. Press button again        → DMA stops, AUDIO:STOP sent to NORA
 *
 * UART PROTOCOL sent to NORA (ESP32):
 *   "AUDIO:START:31250:16:1\n"         — ASCII header (sample rate : bits : channels)
 *   [0xA5 0x5A 0x52 0x45][len_lo len_hi][PCM bytes]   — binary frame per 512 samples
 *   "AUDIO:STOP\n"                     — ASCII end marker
 *
 * HARDWARE:
 *   DFSDM1 Channel 0 / Filter 0
 *   Clock out : PD3  AF3  (DFSDM1_CKOUT  → microphone PDM clock)
 *   Data in   : PD6  AF3  (DFSDM1_DATIN0 → microphone PDM data)
 *   DMA       : DMA1 Stream 1
 *   Sample rate: 100 MHz / 50 (clock div) / 64 (OSR) = 31,250 Hz
 */

#include "audio_rec.h"        /* own header — declares AudioRec_Init, AudioRec_TaskEntry */
#include "audio_sd.h"         /* SD card WAV recording (Stage 1 of voice command pipeline) */
#include "main.h"             /* Error_Handler(), peripheral handles                     */
#include "stm32h7xx_hal.h"    /* all STM32 HAL APIs (DFSDM, DMA, GPIO, UART)            */
#include "FreeRTOS.h"         /* FreeRTOS core types and macros                          */
#include "task.h"             /* vTaskDelay, xTaskCreate                                 */
#include "semphr.h"           /* xSemaphoreCreateBinary, xSemaphoreGiveFromISR           */
#include "queue.h"            /* xQueueCreate, xQueueSendFromISR, xQueueReceive          */
#include <string.h>           /* strlen — used in SendAscii                              */
#include <stdio.h>            /* printf, fflush — Terminal I/O trace                     */

/* ── Configuration ────────────────────────────────────────────────────────────
 * AUDIO_BUF_SAMPLES : number of PCM samples per DMA half-buffer.
 *   At 31,250 Hz / 512 samples = one frame every 16.4 ms — well within
 *   the UART budget (11.2 ms to transmit at 921600 baud).
 * AUDIO_BUF_TOTAL   : total DMA buffer = 2 halves (double-buffer circular DMA).
 * FRAME_MAGIC_*     : 4-byte start-of-frame marker so NORA can re-sync if
 *                     a byte is dropped on UART.
 * ─────────────────────────────────────────────────────────────────────────── */
#define AUDIO_BUF_SAMPLES   512u                      /* PCM samples per DMA half       */
#define AUDIO_BUF_TOTAL     (AUDIO_BUF_SAMPLES * 2u)  /* double-buffer: 2 × 512 = 1024  */
#define FRAME_MAGIC_0       0xA5u                     /* frame sync byte 0              */
#define FRAME_MAGIC_1       0x5Au                     /* frame sync byte 1              */
#define FRAME_MAGIC_2       0x52u                     /* frame sync byte 2  ('R')       */
#define FRAME_MAGIC_3       0x45u                     /* frame sync byte 3  ('E')       */

/* ── Peripheral handles ───────────────────────────────────────────────────────
 * All handles are static (private to this file) — the pattern used by ble_uart.c.
 * Only AudioRec_DMA_IRQHandler() is exposed for the ISR trampoline.
 * ─────────────────────────────────────────────────────────────────────────── */
static DFSDM_Channel_HandleTypeDef s_hdfsdm_ch0;  /* DFSDM channel 0 config + state  */
static DFSDM_Filter_HandleTypeDef  s_hdfsdm_flt0; /* DFSDM filter 0 config + state   */
static DMA_HandleTypeDef           s_hdma_dfsdm;  /* DMA1 Stream1 handle             */

/* ── DMA audio buffer ─────────────────────────────────────────────────────────
 * int32_t because DFSDM outputs 24-bit data in a 32-bit word.
 * __attribute__((aligned(32))) ensures the buffer starts on a 32-byte D-Cache
 * line boundary so SCB_InvalidateDCache_by_Addr() works correctly — if the
 * buffer straddles a cache line, the invalidation would corrupt adjacent data.
 * ─────────────────────────────────────────────────────────────────────────── */
static int32_t s_audioBuf[AUDIO_BUF_TOTAL] __attribute__((aligned(32)));

/* ── FreeRTOS objects ─────────────────────────────────────────────────────────
 * s_buttonSem : binary semaphore — button ISR gives it, task takes it.
 *               Binary (not counting) because we only care that the button
 *               was pressed, not how many times while task was busy.
 * s_halfQueue : queue of uint8_t (0 or 1) — DMA half-complete callback posts
 *               which half of the buffer is ready; task reads and sends it.
 *               Depth 4 gives slack if the task is briefly preempted.
 * ─────────────────────────────────────────────────────────────────────────── */
static SemaphoreHandle_t s_buttonSem;
static QueueHandle_t     s_halfQueue;

/* s_recording : flag shared between task and ISR.
 *   volatile prevents the compiler from caching it in a register across
 *   the while(s_recording) loop, so the ISR-driven stop is always visible. */
static volatile uint8_t s_recording = 0;

/* huart8 : UART8 handle owned by main.c / ble_uart.c.
 *   Declared extern so we can call HAL_UART_Transmit() from here without
 *   moving the handle out of its owner file. */
extern UART_HandleTypeDef huart8;

/* ── Internal helpers ─────────────────────────────────────────────────────────
 *
 * SendAscii — transmits a null-terminated ASCII string over UART8.
 *   Used for AUDIO:START and AUDIO:STOP markers.
 *   100 ms timeout: at 921600 baud, even a 64-byte string takes < 1 ms,
 *   so 100 ms is effectively "never timeout" in normal operation.
 * ─────────────────────────────────────────────────────────────────────────── */
static void SendAscii(const char *str)
{
    HAL_UART_Transmit(&huart8,            /* UART handle                          */
                      (uint8_t *)str,     /* cast: HAL expects uint8_t*           */
                      (uint16_t)strlen(str), /* number of bytes to send           */
                      100);               /* timeout in ms                        */
}

/* ── SendPCMFrame ─────────────────────────────────────────────────────────────
 * Converts 512 int32_t DFSDM samples to int16_t PCM and sends one binary
 * frame over UART8:
 *   [4-byte magic][2-byte length][1024 bytes PCM]
 *
 * Why right-shift by 8?
 *   DFSDM filter sets RightBitShift=8, meaning the useful 16-bit audio is
 *   already in bits [23:8] of each int32 output. Shifting right by 8 extracts
 *   those bits into a int16_t. The bottom 8 bits are noise from the SINC filter.
 *
 * pcm16 is static to avoid allocating 1 KB on the task stack every call.
 * ─────────────────────────────────────────────────────────────────────────── */
static void SendPCMFrame(const int32_t *src32, uint32_t nSamples)
{
    static int16_t pcm16[AUDIO_BUF_SAMPLES] __attribute__((aligned(32)));

    /* Convert each 32-bit DFSDM sample to 16-bit PCM */
    for (uint32_t i = 0; i < nSamples; i++)
    {
        pcm16[i] = (int16_t)(src32[i] >> 8); /* extract 16-bit audio from 32-bit DFSDM word */
    }

    uint16_t byteLen = (uint16_t)(nSamples * sizeof(int16_t)); /* 512 * 2 = 1024 bytes */

    /* Build 6-byte frame header: 4 magic + 2 length (little-endian) */
    uint8_t header[6] = {
        FRAME_MAGIC_0, FRAME_MAGIC_1, FRAME_MAGIC_2, FRAME_MAGIC_3,
        (uint8_t)(byteLen & 0xFF),          /* length low byte  */
        (uint8_t)((byteLen >> 8) & 0xFF)    /* length high byte */
    };

    HAL_UART_Transmit(&huart8, header,           6,       20);  /* send header  */
    HAL_UART_Transmit(&huart8, (uint8_t*)pcm16,  byteLen, 100); /* send payload */
}

/* ── ButtonGPIO_Init ──────────────────────────────────────────────────────────
 * Configures PC13 (blue wakeup button) as a falling-edge EXTI input.
 * The button is active-low: board schematic shows an external pull-up,
 * so the pin is high at rest and falls to GND when pressed.
 * Priority 5: highest FreeRTOS-safe ISR level (same as UART8 ISR).
 * ─────────────────────────────────────────────────────────────────────────── */
static void ButtonGPIO_Init(void)
{
    __HAL_RCC_GPIOC_CLK_ENABLE(); /* enable GPIOC clock so HAL_GPIO_Init works */

    GPIO_InitTypeDef gpio = {0};
    gpio.Pin  = GPIO_PIN_13;            /* PC13 = wakeup/user button             */
    gpio.Mode = GPIO_MODE_IT_FALLING;   /* trigger interrupt on falling edge      */
    gpio.Pull = GPIO_NOPULL;            /* board has external pull-up             */
    HAL_GPIO_Init(GPIOC, &gpio);

    HAL_NVIC_SetPriority(EXTI15_10_IRQn, 5, 0); /* priority 5: FreeRTOS-safe     */
    HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);          /* enable interrupt in NVIC      */
}

/* ── DFSDM_Init ───────────────────────────────────────────────────────────────
 * Initialises DFSDM1 Channel 0 and Filter 0 for PDM microphone input.
 *
 * Clock math:
 *   APB2 = 100 MHz  (DFSDM clock source)
 *   CKOUT = 100 MHz / (2 * (CKOUTDIV+1)) = 100 MHz / (2 * 25) = 2.0 MHz
 *   PCM rate = CKOUT / OSR = 2,000,000 / 64 = 31,250 Hz
 *
 * RightBitShift = 8:
 *   SINC3 filter at OSR=64 produces up to 24-bit output.
 *   Shifting right by 8 places the meaningful 16 bits in the lower half
 *   of the 32-bit DFSDM result register.
 * ─────────────────────────────────────────────────────────────────────────── */
static void DFSDM_Init(void)
{
    /* GPIO: PD3 = DFSDM1_CKOUT (clock to mic), PD6 = DFSDM1_DATIN0 (data from mic) */
    __HAL_RCC_GPIOD_CLK_ENABLE();
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_3 | GPIO_PIN_6; /* PD3 and PD6                       */
    gpio.Mode      = GPIO_MODE_AF_PP;          /* alternate function, push-pull      */
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;     /* high speed: 2 MHz signal on PD3   */
    gpio.Alternate = GPIO_AF3_DFSDM1;          /* both pins use AF3 for DFSDM1      */
    HAL_GPIO_Init(GPIOD, &gpio);

    __HAL_RCC_DFSDM1_CLK_ENABLE(); /* enable DFSDM1 peripheral clock */

    /* ── Channel 0 configuration ────────────────────────────────────────── */
    s_hdfsdm_ch0.Instance = DFSDM1_Channel0;

    /* OutputClock: generate 2 MHz PDM clock on CKOUT pin (PD3) */
    s_hdfsdm_ch0.Init.OutputClock.Activation = ENABLE;
    s_hdfsdm_ch0.Init.OutputClock.Selection  = DFSDM_CHANNEL_OUTPUT_CLOCK_SYSTEM; /* use APB2 */
    s_hdfsdm_ch0.Init.OutputClock.Divider    = 24; /* CKOUT = 100MHz / (2*(24+1)) = 2 MHz */

    /* Input: external PDM data on the channel's own pins (PD6) */
    s_hdfsdm_ch0.Init.Input.Multiplexer  = DFSDM_CHANNEL_EXTERNAL_INPUTS;
    s_hdfsdm_ch0.Init.Input.DataPacking  = DFSDM_CHANNEL_STANDARD_MODE;
    s_hdfsdm_ch0.Init.Input.Pins         = DFSDM_CHANNEL_SAME_CHANNEL_PINS; /* use Channel0 pins */

    /* SPI/PDM interface: sample on rising clock edge (left-channel mic) */
    s_hdfsdm_ch0.Init.SerialInterface.Type     = DFSDM_CHANNEL_SPI_RISING;
    s_hdfsdm_ch0.Init.SerialInterface.SpiClock = DFSDM_CHANNEL_SPI_CLOCK_INTERNAL; /* use CKOUT */

    /* Analog watchdog (not used, but must be initialised) */
    s_hdfsdm_ch0.Init.Awd.FilterOrder   = DFSDM_CHANNEL_FASTSINC_ORDER;
    s_hdfsdm_ch0.Init.Awd.Oversampling  = 10;

    s_hdfsdm_ch0.Init.Offset        = 0; /* no DC offset correction */
    s_hdfsdm_ch0.Init.RightBitShift = 8; /* shift output right 8 bits → 16-bit in lower word */

    if (HAL_DFSDM_ChannelInit(&s_hdfsdm_ch0) != HAL_OK) { Error_Handler(); }

    /* ── Filter 0 configuration ─────────────────────────────────────────── */
    s_hdfsdm_flt0.Instance = DFSDM1_Filter0;

    /* Regular conversion: software trigger, fast mode, DMA-driven */
    s_hdfsdm_flt0.Init.RegularParam.Trigger  = DFSDM_FILTER_SW_TRIGGER; /* start by software      */
    s_hdfsdm_flt0.Init.RegularParam.FastMode = ENABLE;                   /* reduce group delay     */
    s_hdfsdm_flt0.Init.RegularParam.DmaMode  = ENABLE;                   /* DMA reads the results  */

    /* Injected conversion: disabled (not used) */
    s_hdfsdm_flt0.Init.InjectedParam.Trigger        = DFSDM_FILTER_SW_TRIGGER;
    s_hdfsdm_flt0.Init.InjectedParam.ScanMode        = DISABLE;
    s_hdfsdm_flt0.Init.InjectedParam.DmaMode         = DISABLE;
    s_hdfsdm_flt0.Init.InjectedParam.ExtTrigger      = DFSDM_FILTER_EXT_TRIG_TIM1_TRGO;
    s_hdfsdm_flt0.Init.InjectedParam.ExtTriggerEdge  = DFSDM_FILTER_EXT_TRIG_BOTH_EDGES;

    /* Decimation filter: SINC3 at OSR=64 → 31,250 Hz output, good speech quality */
    s_hdfsdm_flt0.Init.FilterParam.SincOrder       = DFSDM_FILTER_SINC3_ORDER; /* 3rd-order SINC */
    s_hdfsdm_flt0.Init.FilterParam.Oversampling    = 64;  /* decimation ratio: 2 MHz / 64 = 31.25 kHz */
    s_hdfsdm_flt0.Init.FilterParam.IntOversampling = 1;   /* integrator OSR = 1 (no extra averaging)  */

    if (HAL_DFSDM_FilterInit(&s_hdfsdm_flt0) != HAL_OK) { Error_Handler(); }

    /* Link Channel 0 to Filter 0 for continuous regular conversion */
    if (HAL_DFSDM_FilterConfigRegChannel(&s_hdfsdm_flt0,
                                          DFSDM_CHANNEL_0,
                                          DFSDM_CONTINUOUS_CONV_ON) != HAL_OK)
    { Error_Handler(); }
}

/* ── DMA_Init ─────────────────────────────────────────────────────────────────
 * Configures DMA1 Stream1 to transfer DFSDM Filter0 results to s_audioBuf.
 *
 * Circular mode: DMA wraps around automatically. HAL fires:
 *   HalfCpltCallback when first half (samples 0..511) is filled
 *   CpltCallback     when second half (samples 512..1023) is filled
 * This lets us process one half while DMA fills the other (double-buffer).
 *
 * __HAL_LINKDMA: attaches this DMA handle to the filter handle so
 *   HAL_DFSDM_FilterRegularStart_DMA knows which stream to use.
 * ─────────────────────────────────────────────────────────────────────────── */
static void DMA_Init(void)
{
    __HAL_RCC_DMA1_CLK_ENABLE(); /* DMA1 clock — may already be on from ble_uart.c, safe to call again */

    s_hdma_dfsdm.Instance                 = DMA1_Stream1;               /* Stream1: free (Stream0 = UART8 RX) */
    s_hdma_dfsdm.Init.Request             = DMA_REQUEST_DFSDM1_FLT0;    /* DMAMUX request 101: DFSDM Filter 0 */
    s_hdma_dfsdm.Init.Direction           = DMA_PERIPH_TO_MEMORY;       /* DFSDM register → RAM              */
    s_hdma_dfsdm.Init.PeriphInc           = DMA_PINC_DISABLE;           /* source address stays fixed        */
    s_hdma_dfsdm.Init.MemInc              = DMA_MINC_ENABLE;            /* destination advances each sample  */
    s_hdma_dfsdm.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;        /* DFSDM result register is 32-bit   */
    s_hdma_dfsdm.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;        /* s_audioBuf[] is int32_t           */
    s_hdma_dfsdm.Init.Mode                = DMA_CIRCULAR;               /* wrap around for double-buffer     */
    s_hdma_dfsdm.Init.Priority            = DMA_PRIORITY_HIGH;          /* audio must not be starved         */
    s_hdma_dfsdm.Init.FIFOMode            = DMA_FIFOMODE_DISABLE;       /* direct mode: lower latency        */

    if (HAL_DMA_Init(&s_hdma_dfsdm) != HAL_OK) { Error_Handler(); }

    __HAL_LINKDMA(&s_hdfsdm_flt0, hdmaReg, s_hdma_dfsdm); /* attach DMA to DFSDM filter handle */

    HAL_NVIC_SetPriority(DMA1_Stream1_IRQn, 5, 0); /* priority 5: FreeRTOS-safe */
    HAL_NVIC_EnableIRQ(DMA1_Stream1_IRQn);
}

/* ── AudioRec_DMA_IRQHandler ──────────────────────────────────────────────────
 * Called from the DMA1_Stream1_IRQHandler trampoline in stm32h7xx_it.c.
 * Delegates to HAL which then calls HalfCplt/CpltCallbacks above.
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioRec_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_dfsdm);
}

/* ── HAL_DFSDM_FilterRegConvHalfCpltCallback ─────────────────────────────────
 * Called by HAL from DMA ISR when first 512 samples are ready (half-complete).
 * 1. Invalidates D-Cache for the first half so the CPU reads fresh DMA data,
 *    not a stale cached copy.
 * 2. Posts index 0 to s_halfQueue to wake the AudioRecTask.
 * portYIELD_FROM_ISR: if the queue post woke a higher-priority task, yield
 * immediately on ISR exit instead of returning to a lower-priority task.
 * ─────────────────────────────────────────────────────────────────────────── */
void HAL_DFSDM_FilterRegConvHalfCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return; /* guard: ignore other filters */

    /* Invalidate cache lines covering s_audioBuf[0..511] */
    SCB_InvalidateDCache_by_Addr((uint32_t*)&s_audioBuf[0],
                                  AUDIO_BUF_SAMPLES * sizeof(int32_t));

    uint8_t idx = 0;                        /* 0 = first half */
    BaseType_t higher = pdFALSE;
    xQueueSendFromISR(s_halfQueue, &idx, &higher);
    portYIELD_FROM_ISR(higher);             /* yield if higher-priority task woken */
}

/* ── HAL_DFSDM_FilterRegConvCpltCallback ─────────────────────────────────────
 * Same as above but for the second 512 samples (DMA transfer complete).
 * ─────────────────────────────────────────────────────────────────────────── */
void HAL_DFSDM_FilterRegConvCpltCallback(DFSDM_Filter_HandleTypeDef *hdfsdm)
{
    if (hdfsdm->Instance != DFSDM1_Filter0) return;

    /* Invalidate cache lines covering s_audioBuf[512..1023] */
    SCB_InvalidateDCache_by_Addr((uint32_t*)&s_audioBuf[AUDIO_BUF_SAMPLES],
                                  AUDIO_BUF_SAMPLES * sizeof(int32_t));

    uint8_t idx = 1;                        /* 1 = second half */
    BaseType_t higher = pdFALSE;
    xQueueSendFromISR(s_halfQueue, &idx, &higher);
    portYIELD_FROM_ISR(higher);
}

/* HAL_GPIO_EXTI_Callback removed — AudioRecTask is disabled.
 * The callback now lives exclusively in voice_recorder.c (VoiceRecTask). */

/* ── AudioRec_Init ────────────────────────────────────────────────────────────
 * Public init function — call once from main() after MX_GPIO_Init().
 * Creates FreeRTOS objects first (before enabling IRQs) so the ISRs
 * always find valid handles. Then inits hardware.
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioRec_Init(void)
{
    s_buttonSem = xSemaphoreCreateBinary(); /* binary: press=1, idle=0           */
    s_halfQueue = xQueueCreate(4, sizeof(uint8_t)); /* 4 slots of 1 byte each    */
    configASSERT(s_buttonSem);              /* halt in debug if allocation failed */
    configASSERT(s_halfQueue);

    ButtonGPIO_Init(); /* configure PC13 EXTI */
    DFSDM_Init();      /* configure DFSDM1 Channel0 + Filter0 */
    DMA_Init();        /* configure DMA1 Stream1 */
}

/* ── AudioRec_TaskEntry ───────────────────────────────────────────────────────
 * FreeRTOS task — runs forever, waiting for button presses.
 *
 * STATE MACHINE:
 *   IDLE:
 *     blocks on s_buttonSem → button press wakes it
 *     debounces 50 ms, drains extra semaphore tokens (bounces)
 *     starts DMA → sends AUDIO:START → enters RECORDING state
 *
 *   RECORDING:
 *     loops: waits for s_halfQueue (DMA half-complete / complete)
 *            reads the ready half → converts int32→int16 → sends UART frame
 *            also polls s_buttonSem: if button pressed → stop
 *
 *   STOP (inside RECORDING loop):
 *     stops DMA → sends AUDIO:STOP → drains leftover queue → goes back to IDLE
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioRec_TaskEntry(void *arg)
{
    (void)arg; /* unused parameter — suppress compiler warning */

    for (;;)
    {
        /* ── IDLE: wait for first button press ─────────────────────────── */
        xSemaphoreTake(s_buttonSem, portMAX_DELAY); /* block until button pressed */
        vTaskDelay(pdMS_TO_TICKS(50));              /* wait 50 ms for debounce   */
        while (xSemaphoreTake(s_buttonSem, 0) == pdTRUE) {} /* drain bounce tokens */

        printf("[AUDIO] Button pressed — starting recording\n"); fflush(stdout);

        /* Start recording */
        s_recording = 1;
        SendAscii("AUDIO:START:31250:16:1\n");             /* notify NORA: recording begins */
        printf("[AUDIO] AUDIO:START sent to NORA\n"); fflush(stdout);
        AudioSD_StartRecording(NULL, 0);                    /* open REC_NNN.wav on SD card   */
        printf("[AUDIO] SD card recording started\n"); fflush(stdout);
        HAL_DFSDM_FilterRegularStart_DMA(&s_hdfsdm_flt0,   /* start DMA conversion          */
                                         s_audioBuf,        /* destination buffer            */
                                         AUDIO_BUF_TOTAL);  /* total elements (1024 int32)   */

        /* ── RECORDING: drain DMA frames and send over UART ────────────── */
        uint8_t halfIdx; /* which half of s_audioBuf is ready: 0 = first, 1 = second */

        while (s_recording)
        {
            /* Wait up to 200 ms for a DMA half-complete event.
             * 200 ms >> 16.4 ms frame period, so a timeout means something is wrong. */
            if (xQueueReceive(s_halfQueue, &halfIdx, pdMS_TO_TICKS(200)) == pdTRUE)
            {
                /* Select the correct half of the buffer */
                const int32_t *src = (halfIdx == 0)
                    ? &s_audioBuf[0]               /* first half:  samples 0..511   */
                    : &s_audioBuf[AUDIO_BUF_SAMPLES]; /* second half: samples 512..1023 */

                SendPCMFrame(src, AUDIO_BUF_SAMPLES); /* convert + transmit over UART8 */
                AudioSD_WriteFrame(src, AUDIO_BUF_SAMPLES); /* also write to SD card    */
            }

            /* Check if button was pressed again to stop recording.
             * Non-blocking take (timeout=0): only stops if already given. */
            if (xSemaphoreTake(s_buttonSem, 0) == pdTRUE)
            {
                vTaskDelay(pdMS_TO_TICKS(50));              /* debounce */
                while (xSemaphoreTake(s_buttonSem, 0) == pdTRUE) {} /* drain bounces */

                printf("[AUDIO] Button pressed — stopping recording\n"); fflush(stdout);
                HAL_DFSDM_FilterRegularStop_DMA(&s_hdfsdm_flt0); /* stop DMA          */
                s_recording = 0;                                   /* exit the loop     */
                SendAscii("AUDIO:STOP\n");                         /* notify NORA: done */
                printf("[AUDIO] AUDIO:STOP sent to NORA\n"); fflush(stdout);

                /* Drain any DMA events that arrived between stop and here */
                uint8_t dummy;
                while (xQueueReceive(s_halfQueue, &dummy, 0) == pdTRUE) {}

                /* Finalise the WAV file: patch header, close, then send to NORA */
                char sdFilename[16];
                if (AudioSD_StopRecording(sdFilename, sizeof(sdFilename)))
                {
                    printf("[AUDIO] Sending SD file [%s] to NORA\n", sdFilename); fflush(stdout);
                    AudioSD_SendFileToUART(sdFilename); /* streams AUDIO:FILE: + bytes */
                    printf("[AUDIO] File send complete\n"); fflush(stdout);
                }

                break; /* exit while(s_recording) and go back to IDLE */
            }
        }
    }
}
