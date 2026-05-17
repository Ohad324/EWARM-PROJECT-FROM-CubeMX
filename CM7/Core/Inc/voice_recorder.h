/*
 * voice_recorder.h — 5-second button-triggered voice recorder
 *
 * TRIGGER  : Blue button PC13 EXTI → VoiceRecTask
 * HARDWARE : DFSDM1 Filter0, DMA1 Stream1, 16 kHz 16-bit mono
 * STORE    : SDRAM g_AudioBuf (160 KB) → SDWriteTask → REC_NNN.wav on SD
 * LOG      : RTTLogTask → SEGGER RTT channel 0
 */

#ifndef VOICE_RECORDER_H
#define VOICE_RECORDER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>     /* size_t for WakeWord_GetWindow() */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* ── Exit codes ──────────────────────────────────────────────────────────── */
typedef enum {
    REC_OK               = 0,
    REC_ERR_DMA_START    = 1,
    REC_ERR_DMA_OVERFLOW = 2,
    REC_ERR_DMA_STOP     = 3,
    REC_ERR_SD_MOUNT     = 4,
    REC_ERR_SD_OPEN      = 5,
    REC_ERR_SD_WRITE_HDR = 6,
    REC_ERR_SD_WRITE_PCM = 7,
    REC_ERR_SD_CLOSE     = 8,
    REC_ERR_QUEUE_FULL   = 9,
} REC_Result_t;

/* ── Recording FSM state ─────────────────────────────────────────────────── */
typedef enum {
    REC_IDLE      = 0,
    REC_RECORDING = 1,
    REC_SAVING    = 2,
} RecState_t;

/* ── Log entry posted to xLogQueue by any task → printed by RTTLogTask ───── */
typedef struct {
    uint32_t    timestamp;   /* HAL_GetTick() ms since boot — wraps every 49 days */
    const char *pcMsg;       /* pointer to string literal in flash — only 4 bytes copied */
    uint32_t    val;         /* optional numeric value (error code, byte count, etc.)    */
} LogMsg_t;

/* ── Public API ──────────────────────────────────────────────────────────── */

/* Lightweight bring-up init — button GPIO + NVIC only, no DFSDM/DMA.
 * Use this to verify button + RTT before enabling full recording. */
void VoiceRec_ButtonInit(void);

/* Full init — call once from main() after AudioSD_Init(), before osKernelStart() */
void VoiceRec_Init(void);

/* Query current FSM state — safe to call from any context */
RecState_t VoiceRec_GetState(void);

/* FreeRTOS task entries — pass to xTaskCreate */
void VoiceRecTask(void *arg);   /* arg = QueueHandle_t xVoiceQueue */
void SDWriteTask(void *arg);    /* arg = QueueHandle_t xVoiceQueue */
/* RTTLogTask declared in rtt_log_task.h */

/* IRQ trampoline — called from DMA1_Stream1_IRQHandler in stm32h7xx_it.c */
void VoiceRec_DMA_IRQHandler(void);

/* Phase 2a (2026-05-17): rolling wake-word window accessor.
 * Copies the most-recent n_samples from the continuous DFSDM PCM stream
 * into `out`, in chronological order (oldest to newest). Handles circular
 * wrap internally. Returns number of samples actually copied (clamped to
 * the underlying buffer size, 16000). Safe to call from any context.
 * Phase 3's idle-hook classifier will use this to fetch the latest 1 sec
 * of audio every ~500 ms. */
size_t WakeWord_GetWindow(int16_t *out, size_t n_samples);

/* ── Audio health monitor ────────────────────────────────────────────────── */
typedef struct {
    uint32_t dfsdm_overruns;      /* hardware overrun count — should always be 0 */
    uint32_t sd_write_max_ms;     /* worst-case f_write duration in milliseconds  */
    uint32_t buffer_misses;       /* f_write errors or short writes               */
    uint32_t total_bytes_written; /* running total for progress tracking          */
} AudioHealth_t;

extern volatile AudioHealth_t g_AudioHealth;

/* ── Globals shared with main.c / rtt_log_task.c ────────────────────────── */
extern TaskHandle_t       voiceRecTaskHandle;   /* used by button ISR       */
extern volatile uint32_t  g_sdFreeKB;           /* updated after each f_close */

#ifdef __cplusplus
}
#endif

#endif /* VOICE_RECORDER_H */
