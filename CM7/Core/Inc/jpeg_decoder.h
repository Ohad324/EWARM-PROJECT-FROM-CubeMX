/**
 * @file  jpeg_decoder.h
 * @brief HAL_JPEG hardware decode + scale to display resolution.
 *
 * Pipeline:
 *   HAL_JPEG_Decode()               -- hardware, uses MDMA (hjpeg from main.c)
 *   JPEG_GetDecodeColorConvertFunc() -- jpeg_utils.c software MCU->RGB888
 *   scale_rgb888()                  -- nearest-neighbour CPU scale to 800x480
 *   SCB_CleanDCache_by_Addr()       -- flush CPU writes before DMA2D reads
 *
 * All intermediate and output buffers live in SDRAM (.sdram_bss section).
 * Input buffer (s_jpegInBuf) is also in SDRAM (.sdram_bss); MDMA can reach it.
 */
#ifndef JPEG_DECODER_H
#define JPEG_DECODER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "stm32h7xx_hal.h"

/** Initialise jpeg_utils colour-conversion lookup tables.
 *  Call once before the first JPEG_Decode(). */
void JPEG_Init(void);

/**
 * @brief Decode a JPEG image to RGB888, scaled to 800x480.
 *
 * @param jpegData   Pointer to raw JPEG bytes in SDRAM (DMA-accessible).
 *                   Caller must not modify the buffer until this returns.
 * @param jpegSize   Number of bytes in jpegData.
 * @param outWidth   Decoded image width  in pixels (output parameter).
 * @param outHeight  Decoded image height in pixels (output parameter).
 * @return HAL_OK on success, HAL_ERROR on any failure.
 */
HAL_StatusTypeDef JPEG_Decode(const uint8_t *jpegData, uint32_t jpegSize,
                               uint32_t *outWidth, uint32_t *outHeight);

/** Pointer to the 800x480 RGB888 scaled output buffer in SDRAM.
 *  Valid (and D-Cache clean) after a successful JPEG_Decode().
 *  Contents are replaced by the next call to JPEG_Decode().
 *  Pass directly to PixelDataWidget::setPixelData() with Bitmap::RGB888. */
uint8_t *JPEG_GetRGB888Buffer(void);

#ifdef __cplusplus
}
#endif

#endif /* JPEG_DECODER_H */
