/*
 * audio_rec.h — Button-triggered MEMS microphone recording
 *
 * Press blue wakeup button (PC13) → start recording
 * Press again                     → stop recording, flush to NORA via UART8
 *
 * DFSDM1 Channel 0 / Filter 0
 *   Clock out : PD3  AF3 (DFSDM1_CKOUT)
 *   Data in   : PD6  AF3 (DFSDM1_DATIN0)
 * DMA         : DMA1_Stream1
 * Sample rate : ~31,250 Hz  (100 MHz / 50 / 64 OSR)
 * Format      : 16-bit signed PCM, mono
 */
#ifndef AUDIO_REC_H
#define AUDIO_REC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call once from main(), after MX_GPIO_Init() and BLE_UART_Init() */
void AudioRec_Init(void);

/* Task entry — pass to xTaskCreate */
void AudioRec_TaskEntry(void *arg);

/* IRQ trampolines — called from stm32h7xx_it.c */
void AudioRec_DMA_IRQHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_REC_H */
