#include "jpeg_decoder.h"
#include "jpeg_utils.h"     /* JPEG_InitColorTables, JPEG_GetDecodeColorConvertFunc */
#include "log_mutex.h"
#include "timing_log.h"     /* TLOG(), T_US() */
#include "main.h"           /* hjpeg */
#include "SEGGER_RTT.h"     /* SEGGER_RTT_Write() for pixel buffer dump */
#include "FreeRTOS.h"
#include <string.h>

#define DWT_MS(c)  ((c) / 480000UL)
#define DWT_SNAP() (DWT->CYCCNT)

/* BP() removed — BKPT without a live debug session causes HardFault+reset */

extern JPEG_HandleTypeDef hjpeg;

/* ── Display geometry ────────────────────────────────────────────────────── */
#define DISPLAY_W  800u
#define DISPLAY_H  480u

/* ── SDRAM buffers ────────────────────────────────────────────────────────
 *
 *  s_ycbcrBuf   : raw MCU output from HAL_JPEG_Decode.
 *                 Sized for 1280x720 4:2:0 worst-case:
 *                 ceil(1280/16) x ceil(720/16) = 80x45 = 3600 MCUs x 384 B
 *                 = 1 382 400 B.  Using 1280x720x2 = 1 843 200 B as margin.
 *
 *  s_rgb888Buf  : intermediate full-res RGB888 from jpeg_utils conversion.
 *                 1280x720x3 = 2 764 800 B
 *
 *  s_rgb888Scaled: nearest-neighbour downscale to 800x480 (display size).
 *                 800x480x3 = 1 152 000 B  ← what PixelDataWidget reads.
 *
 *  Total SDRAM: ~5.76 MB out of 32 MB.
 *  32-byte aligned for SCB_CleanDCache_by_Addr().
 * ──────────────────────────────────────────────────────────────────────── */
static uint8_t s_ycbcrBuf[1280u * 720u * 2u]
    __attribute__((section(".sdram_bss"), aligned(32)));

static uint8_t s_rgb888Buf[1280u * 720u * 3u]
    __attribute__((section(".sdram_bss"), aligned(32)));

static uint8_t s_rgb888Scaled[DISPLAY_W * DISPLAY_H * 3u]
    __attribute__((section(".sdram_bss"), aligned(32)));

/* ── Nearest-neighbour scale: src(srcW x srcH RGB888) → dst(dstW x dstH) ── */
static void scale_rgb888(const uint8_t *src, uint32_t srcW, uint32_t srcH,
                          uint8_t *dst,       uint32_t dstW, uint32_t dstH)
{
    /* Pre-compute source X byte-offsets for every destination column.
     * Eliminates the UDIV from the inner loop: dstW divisions instead of
     * dstW*dstH.  For 800x480 output: 800 divisions vs 384,000.
     * Stack cost: dstW * 2 bytes (800 * 2 = 1600 B) — caller must have room. */
    uint16_t sx_lut[DISPLAY_W];   /* DISPLAY_W = 800, byte offset = sx * 3 */
    for (uint32_t dx = 0u; dx < dstW; dx++)
        sx_lut[dx] = (uint16_t)((dx * srcW / dstW) * 3u);

    for (uint32_t dy = 0u; dy < dstH; dy++)
    {
        uint32_t       sy      = (dy * srcH) / dstH;
        const uint8_t *src_row = src + sy * srcW * 3u;
        uint8_t       *dst_row = dst + dy * dstW * 3u;
        for (uint32_t dx = 0u; dx < dstW; dx++)
        {
            uint32_t sp          = sx_lut[dx];
            uint32_t dp          = dx * 3u;
            dst_row[dp]          = src_row[sp];
            dst_row[dp + 1u]     = src_row[sp + 1u];
            dst_row[dp + 2u]     = src_row[sp + 2u];
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void JPEG_Init(void)
{
    JPEG_InitColorTables();
    LOG("[MUSIC] JPEG colour tables initialised\n");

    LOG("[MEM] s_ycbcrBuf=%p %luKB  s_rgb888Buf=%p %luKB  s_rgb888Scaled=%p %luKB\n",
        (void *)s_ycbcrBuf,     (uint32_t)(sizeof(s_ycbcrBuf)     / 1024u),
        (void *)s_rgb888Buf,    (uint32_t)(sizeof(s_rgb888Buf)    / 1024u),
        (void *)s_rgb888Scaled, (uint32_t)(sizeof(s_rgb888Scaled) / 1024u));
}

/** Returns the 800x480 RGB888 buffer ready for PixelDataWidget. */
uint8_t *JPEG_GetRGB888Buffer(void)
{
    return s_rgb888Scaled;
}

HAL_StatusTypeDef JPEG_Decode(const uint8_t *jpegData, uint32_t jpegSize,
                               uint32_t *outWidth, uint32_t *outHeight)
{
    LOG("[JPEG] Stage1: input=%p size=%lu  ycbcr_buf=%p size=%lu\n",
        (void *)jpegData, jpegSize,
        (void *)s_ycbcrBuf, (uint32_t)sizeof(s_ycbcrBuf));

    /* ── 1. D-Cache: invalidate SDRAM input & YCbCr buffers ───────────── *
     * jpegData is in SDRAM (.sdram_bss).  UARTReceiveTask wrote it via CPU
     * and already called SCB_CleanDCache_by_Addr() on it, but we invalidate
     * here defensively so the JPEG hardware reads fresh SDRAM data.        */
    uint32_t alignedSize = (jpegSize + 31u) & ~31u;
    SCB_InvalidateDCache_by_Addr((uint32_t *)(uintptr_t)jpegData,
                                  (int32_t)alignedSize);
    SCB_InvalidateDCache_by_Addr((uint32_t *)s_ycbcrBuf,
                                  (int32_t)sizeof(s_ycbcrBuf));

    /* ── 2. Hardware JPEG decode -> MCU YCbCr blocks ─────────────────── */
    LOG("[MUSIC] JPEG decode started\n");
    HAL_StatusTypeDef st = HAL_JPEG_Decode(
        &hjpeg,
        (uint8_t *)(uintptr_t)jpegData, jpegSize,
        s_ycbcrBuf, (uint32_t)sizeof(s_ycbcrBuf),
        HAL_MAX_DELAY);

    if (st != HAL_OK)
    {
        LOG("[JPEG] Stage1 FAILED: HAL_JPEG_Decode err=%d\n", (int)st);
        return HAL_ERROR;
    }

    JPEG_ConfTypeDef jpegInfo;
    HAL_JPEG_GetInfo(&hjpeg, &jpegInfo);
    *outWidth  = jpegInfo.ImageWidth;
    *outHeight = jpegInfo.ImageHeight;
    LOG("[MUSIC] JPEG decode complete: %lu x %lu  subsampling=%lu\n",
        *outWidth, *outHeight, (uint32_t)jpegInfo.ChromaSubsampling);

    /* Post-decode: re-invalidate s_ycbcrBuf in case MDMA wrote to SDRAM
     * without updating the D-Cache (bypass). */
    SCB_InvalidateDCache_by_Addr((uint32_t *)s_ycbcrBuf,
                                  (int32_t)sizeof(s_ycbcrBuf));

    LOG("[JPEG] Stage2: %lux%lu cs=%lu ss=%lu  ycbcr[0..7]=%02X%02X%02X%02X%02X%02X%02X%02X\n",
        jpegInfo.ImageWidth, jpegInfo.ImageHeight,
        (uint32_t)jpegInfo.ColorSpace, (uint32_t)jpegInfo.ChromaSubsampling,
        s_ycbcrBuf[0], s_ycbcrBuf[1], s_ycbcrBuf[2], s_ycbcrBuf[3],
        s_ycbcrBuf[4], s_ycbcrBuf[5], s_ycbcrBuf[6], s_ycbcrBuf[7]);

    /* ── 3. Software MCU YCbCr -> RGB888 via jpeg_utils ─────────────── */
    JPEG_YCbCrToRGB_Convert_Function convertFn;
    uint32_t nMCUs = 0u;
    LOG("[MUSIC] calling JPEG_GetDecodeColorConvertFunc...\n");
    HAL_StatusTypeDef ccSt = JPEG_GetDecodeColorConvertFunc(&jpegInfo, &convertFn, &nMCUs);
    LOG("[MUSIC] JPEG_GetDecodeColorConvertFunc returned: %d  fn=%p  nMCUs=%lu\n",
        (int)ccSt, (void*)convertFn, nMCUs);
    if (ccSt != HAL_OK)
    {
        LOG("[MUSIC] JPEG_GetDecodeColorConvertFunc FAILED  subsampling=%lu\n",
            (uint32_t)jpegInfo.ChromaSubsampling);
        return HAL_ERROR;
    }
    /* MCU input block size (bytes) based on chroma subsampling:
     *   4:2:0 -> 6 x 64 = 384 B/MCU  (16x16 pixel MCU)
     *   4:2:2 -> 4 x 64 = 256 B/MCU  ( 8x16 pixel MCU)
     *   4:4:4 -> 3 x 64 = 192 B/MCU  ( 8x8  pixel MCU) */
    uint32_t mcuInBytes;
    switch (jpegInfo.ChromaSubsampling)
    {
        case JPEG_420_SUBSAMPLING: mcuInBytes = 384u; break;
        case JPEG_422_SUBSAMPLING: mcuInBytes = 256u; break;
        default:                   mcuInBytes = 192u; break; /* 4:4:4 */
    }
    LOG("[MUSIC] ColorConvert OK: fn=%p  nMCUs=%lu  mcuBytes=%lu\n",
        (void*)convertFn, nMCUs, mcuInBytes);

    const uint8_t *pIn  = s_ycbcrBuf;
    uint8_t       *pOut = s_rgb888Buf;

    LOG("[JPEG] Stage3: pIn=%p pOut=%p nMCUs=%lu mcuBytes=%lu fn=%p\n",
        (void *)pIn, (void *)pOut, nMCUs, mcuInBytes, (void *)convertFn);

    uint32_t t8a_us = T_US();
    TLOG("8a MCU_RGB_start  nMCUs=%lu  mcuBytes=%lu  t=%lu us", nMCUs, mcuInBytes, t8a_us);
    uint32_t t_conv = DWT_SNAP();
    for (uint32_t mcu = 0u; mcu < nMCUs; mcu++)
    {
        if (mcu % 100u == 0u)
            LOG("[JPEG] MCU %lu/%lu\n", mcu, nMCUs);

        uint32_t converted = 0u;
        convertFn((uint8_t *)pIn, pOut, mcu, mcuInBytes, &converted);
        pIn  += mcuInBytes;
        pOut += converted;
    }
    {
        uint32_t t8b_us = T_US();
        TLOG("8b MCU_RGB_done  t=%lu us  dur=%lu us", t8b_us, t8b_us - t8a_us);
    }

    LOG("[JPEG] Stage4: YCbCr->RGB DONE  rgb888=%p\n", (void *)s_rgb888Buf);

    /* ── 4. Nearest-neighbour scale to display resolution ─────────────── *
     * Input : s_rgb888Buf  (*outWidth x *outHeight)  — e.g. 1280x720      *
     * Output: s_rgb888Scaled (DISPLAY_W x DISPLAY_H) — 800x480            *
     * PixelDataWidget renders s_rgb888Scaled 1:1 onto the framebuffer.    */
    LOG("[JPEG] Stage5: scale %lux%lu->%ux%u  src=%p dst=%p\n",
        *outWidth, *outHeight, DISPLAY_W, DISPLAY_H,
        (void *)s_rgb888Buf, (void *)s_rgb888Scaled);

    uint32_t t_scale = DWT_SNAP();
    scale_rgb888(s_rgb888Buf, *outWidth, *outHeight,
                 s_rgb888Scaled, DISPLAY_W, DISPLAY_H);
    LOG("[MUSIC] Scale %lux%lu->%ux%u: %lu ms / %lu Kcycles\n",
        *outWidth, *outHeight, DISPLAY_W, DISPLAY_H,
        DWT_MS(DWT_SNAP() - t_scale), (DWT_SNAP() - t_scale) / 1000UL);

    LOG("[JPEG] Stage6: scaling DONE  scaled=%p\n", (void *)s_rgb888Scaled);

#ifndef RELEASE_BUILD
    /* DEBUG — R<->B swap probe. STM32H7 LTDC PF=RGB888 reads memory in
     * [B,G,R] byte order, but jpeg_utils writes [R,G,B]. Swap in place so
     * the buffer LTDC sees has the channels lined up correctly.
     * If the panel still shows wrong colors after this, the conversion
     * function output order is different than assumed -- iterate. */
    {
        uint8_t *p = s_rgb888Scaled;
        const uint32_t pixels = DISPLAY_W * DISPLAY_H;
        for (uint32_t i = 0u; i < pixels; i++)
        {
            uint8_t r = p[0];
            p[0] = p[2];
            p[2] = r;
            p += 3;
        }
    }
#endif

    /* ── 5. D-Cache clean: flush CPU writes to SDRAM so DMA2D can read ── */
    SCB_CleanDCache_by_Addr((uint32_t *)s_rgb888Scaled,
                             (int32_t)sizeof(s_rgb888Scaled));

    /* Pixel sanity: log first two pixels for quick verification via RTT */
    LOG("[MUSIC] Pixel[0] #%02X%02X%02X  Pixel[1] #%02X%02X%02X\n",
        s_rgb888Scaled[0], s_rgb888Scaled[1], s_rgb888Scaled[2],
        s_rgb888Scaled[3], s_rgb888Scaled[4], s_rgb888Scaled[5]);

#ifdef ENABLE_RTT_DUMP
    /* ── SEGGER RTT binary pixel dump ─────────────────────────────────── *
     * Streams the full 800x480 RGB888 buffer (1 152 000 bytes) to the    *
     * J-Link RTT Logger on the host PC.                                   *
     *                                                                     *
     * To capture:                                                         *
     *   JLinkRTTLogger.exe -device STM32H747XI -if SWD -speed 4000       *
     *       -RTTChannel 0 dump.raw                                        *
     *                                                                     *
     * To view on PC (Python):                                             *
     *   from PIL import Image                                             *
     *   data = open("dump.raw","rb").read()[:800*480*3]                   *
     *   Image.frombytes("RGB",(800,480),data).show()                      *
     *                                                                     *
     * Runs in BLOCK_IF_FIFO_FULL mode — JpegDisplayTask stalls ~0.3 s   *
     * while the host drains the pipe.  Disable by removing the           *
     * ENABLE_RTT_DUMP preprocessor symbol when not debugging.            */
    LOG("[MUSIC] RTT dump start: %u bytes\n",
        (unsigned)sizeof(s_rgb888Scaled));
    SEGGER_RTT_Write(0u, s_rgb888Scaled, sizeof(s_rgb888Scaled));
    LOG("[MUSIC] RTT dump complete\n");
#endif /* ENABLE_RTT_DUMP */

    return HAL_OK;
}
