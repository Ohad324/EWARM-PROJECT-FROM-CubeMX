/* cache-bust: 2026-04-14 */
/*
 * audio_sd.h — SD card WAV recording layer
 *
 * Sits between audio_rec.c (DMA/DFSDM) and FatFS.
 * Call AudioSD_Init() once before the scheduler starts.
 * Call AudioSD_StartRecording() on button-press-to-start.
 * Call AudioSD_WriteFrame() from AudioRec_TaskEntry for every DMA half.
 * Call AudioSD_StopRecording() on button-press-to-stop — returns filename.
 * Call AudioSD_SendFileToUART() to stream the finished WAV to NORA.
 *
 * SD CARD HARDWARE (STM32H747I-DISCO):
 *   SDMMC1_D0  : PC8   AF12
 *   SDMMC1_D1  : PC9   AF12
 *   SDMMC1_D2  : PC10  AF12
 *   SDMMC1_D3  : PC11  AF12
 *   SDMMC1_CK  : PC12  AF12
 *   SDMMC1_CMD : PD2   AF12
 *   SD_DETECT  : PI8   (active-low, GPIO input, external pull-up)
 */

#ifndef AUDIO_SD_H
#define AUDIO_SD_H

#include <stdint.h>
#include <stdbool.h>

/* ── Public API ────────────────────────────────────────────────────────────── */

/*
 * AudioSD_Init — initialise SDMMC1 peripheral, mount FatFS volume.
 * Call once from main() after MX_GPIO_Init(), before osKernelStart().
 * Returns true on success; false if card absent or mount fails.
 */
bool AudioSD_Init(void);

/*
 * AudioSD_StartRecording — open a new REC_NNN.wav file, write placeholder header.
 * Returns true if the file was opened successfully.
 * filename_out: if non-NULL, receives the name chosen (e.g. "REC_001.wav").
 */
bool AudioSD_StartRecording(char *filename_out, uint8_t maxLen);

/*
 * AudioSD_WriteFrame — convert nSamples int32 DFSDM words to int16 PCM
 * and append to the open WAV file.
 * Must only be called between AudioSD_StartRecording and AudioSD_StopRecording.
 */
void AudioSD_WriteFrame(const int32_t *src32, uint32_t nSamples);

/*
 * AudioSD_StopRecording — stop recording: patch WAV header with final sizes,
 * flush and close the file.
 * filename_out: receives the closed filename (e.g. "REC_001.wav"), maxLen bytes.
 * Returns true on success.
 */
bool AudioSD_StopRecording(char *filename_out, uint8_t maxLen);

/*
 * AudioSD_SendFileToUART — read the named file from SD and transmit it to NORA
 * over UART8 using the protocol:
 *   "AUDIO:FILE:REC_001.wav:<byteCount>\n"   ASCII header
 *   <byteCount raw bytes>                    binary WAV payload
 * Returns true if the transfer completed without error.
 */
bool AudioSD_SendFileToUART(const char *filename);

/*
 * AudioSD_Remount — recover SDMMC peripheral and re-mount FatFS.
 * Call after AudioSD_SendFileToUART() or after any SDMMC error to restore
 * the SD card to a usable state for the next recording.
 * Returns true if the volume was re-mounted successfully.
 */
bool AudioSD_Remount(void);

/*
 * AudioSD_Format — format the SD card as exFAT and mount the new volume.
 * Call when f_open returns FR_NO_FILESYSTEM (blank or unrecognised card).
 * Returns true if the volume is mounted and ready.
 */
bool AudioSD_Format(void);

/*
 * AudioSD_DeInit — unmount FatFS and release SDMMC1 peripheral.
 * Call before USB MSC takes ownership of SDMMC1.
 */
void AudioSD_DeInit(void);

/*
 * AudioSD_SDMMC_IRQHandler — trampoline; call from SDMMC1_IRQHandler in stm32h7xx_it.c.
 */
void AudioSD_SDMMC_IRQHandler(void);

/*
 * AudioSD_GetErrorCode — returns s_hsd1.ErrorCode for health monitoring.
 * 0 = no error since last reset.
 */
uint32_t AudioSD_GetErrorCode(void);


/*
 * AudioSD_IsBusy — returns true while AudioSD_SendFileToUART() is active.
 * Callers should avoid FatFS operations while busy — concurrent access
 * corrupts win[] and causes FR_DISK_ERR mid-read (B-008).
 */
bool AudioSD_IsBusy(void);

/*
 * AudioSD_NotifyReady — called by routeAsciiMessage() in main.c when
 * "AUDIO:READY" is received from NORA over UART8.
 * Signals AudioSD_SendFileToUART() to start streaming WAV bytes.
 * Must be safe to call from any task context.
 */
void AudioSD_NotifyReady(void);

#endif /* AUDIO_SD_H */
