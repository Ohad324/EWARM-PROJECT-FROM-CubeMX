/*
 * audio_sd.c — SD card WAV recording via SDMMC1 + FatFS
 *
 * HOW IT WORKS:
 *   1. AudioSD_Init()           → init SDMMC1 GPIO/peripheral, mount FatFS volume
 *   2. AudioSD_StartRecording() → choose next REC_NNN filename, open file,
 *                                  write a 44-byte placeholder WAV header
 *   3. AudioSD_WriteFrame()     → called per DMA half-complete: shift int32→int16,
 *                                  f_write() 1 KB chunk to the open file
 *   4. AudioSD_StopRecording()  → seek to byte 0, overwrite header with real sizes,
 *                                  f_close() the file
 *   5. AudioSD_SendFileToUART() → re-open file, send ASCII header then raw bytes
 *                                  to NORA over UART8
 *
 * SD CARD HARDWARE (STM32H747I-DISCO — see board schematic DS12556):
 *   SDMMC1_D0  : PC8   AF12
 *   SDMMC1_D1  : PC9   AF12
 *   SDMMC1_D2  : PC10  AF12
 *   SDMMC1_D3  : PC11  AF12
 *   SDMMC1_CK  : PC12  AF12
 *   SDMMC1_CMD : PD2   AF12
 *   SD_DETECT  : PI8   GPIO_INPUT (active-low, external pull-up)
 *
 * FATFS:
 *   Logical drive "0:" mapped to SDMMC1.
 *   Sector size: 512 bytes.  Max file size: limited by FAT32 (4 GB).
 *   ff.c / diskio.c are expected to be present in Middlewares/Third_Party/FatFs.
 *   This file provides the low-level disk_read / disk_write / disk_ioctl callbacks
 *   via sd_diskio_impl_* helpers defined at the bottom.
 *
 * ASSUMPTIONS:
 *   - FatFS volume "0:" is dedicated to SDMMC1 (ff_conf.h: FF_VOLUMES >= 1).
 *   - The FatFS work area is statically allocated here.
 *   - SDMMC1 IDMA requires the sector buffer to be in AXI SRAM (not DTCM).
 *   - huart8 is defined in main.c / ble_uart.c and is accessible extern.
 */

#include "audio_sd.h"
#include "main.h"           /* Error_Handler, peripheral handles */
#include "stm32h7xx_hal.h"  /* HAL_SD_*, HAL_GPIO_Init, etc.    */
#include "ff.h"             /* FatFS f_open, f_write, f_close   */
#include "diskio.h"         /* DRESULT, DSTATUS — FatFS disk I/O */
#include "FreeRTOS.h"
#include "task.h"           /* vTaskDelay                        */
#include "log_mutex.h"      /* SEGGER_RTT_printf, RTT_TS         */
#include "itm_log.h"        /* STAGE() — ITM + RTT, no printf    */
#include <string.h>         /* memcpy, strlen                    */
#include <stdio.h>          /* snprintf                          */

/* FatFS multi-partition map — required when FF_MULTI_PARTITION == 1.
 * Maps logical drive "0:" → physical drive 0, MBR partition 1.
 * Any standard Windows format (Explorer / diskpart / USB MSC) creates
 * MBR at sector 0 with exFAT starting at sector 2048. FatFS reads the
 * MBR, finds partition 1, and mounts from there. */
PARTITION VolToPart[FF_VOLUMES] = {
    {0, 1}   /* "0:" → physical drive 0, partition 1 */
};

/* Timestamped RTT log line for this module — avoids SendUartStr on UART8.
 * Uses SEGGER_RTT_Write (always linked) rather than SEGGER_RTT_printf
 * (only in SEGGER_RTT_printf.c, not compiled in this project). */
#define SD_LOG(fmt, ...) do { \
    char _sd_buf[80]; \
    int  _sd_n = snprintf(_sd_buf, sizeof(_sd_buf), \
                          "[T+%7lu] [SD] " fmt "\r\n", \
                          (unsigned long)HAL_GetTick(), ##__VA_ARGS__); \
    if (_sd_n > 0) SEGGER_RTT_Write(0, _sd_buf, (unsigned)_sd_n); \
    if (_sd_n > 0) { printf("%s", _sd_buf); } \
} while (0)

/* ── Configuration ───────────────────────────────────────────────────────────
 * AUDIO_SD_SAMPLES_PER_FRAME : must match AUDIO_BUF_SAMPLES in audio_rec.c
 * AUDIO_SD_UART_CHUNK        : bytes per UART send call when streaming the file
 * AUDIO_SD_MAX_REC_NUM       : highest REC_NNN index before wrapping to 001
 * ─────────────────────────────────────────────────────────────────────────── */
#define AUDIO_SD_SAMPLES_PER_FRAME  512u
#define AUDIO_SD_UART_CHUNK         1024u
#define AUDIO_SD_MAX_REC_NUM        999u
#define AUDIO_SD_SAMPLE_RATE        31250u
#define AUDIO_SD_BITS_PER_SAMPLE    16u
#define AUDIO_SD_CHANNELS           1u

/* ── WAV header (44 bytes, little-endian, packed) ─────────────────────────── */
typedef struct __attribute__((packed))
{
    char     riff[4];           /* "RIFF"                              */
    uint32_t chunkSize;         /* fileSize - 8                        */
    char     wave[4];           /* "WAVE"                              */
    char     fmt[4];            /* "fmt "                              */
    uint32_t subchunk1Size;     /* 16 for PCM                          */
    uint16_t audioFormat;       /* 1 = PCM                             */
    uint16_t numChannels;       /* 1 = mono                            */
    uint32_t sampleRate;        /* 31250                               */
    uint32_t byteRate;          /* sampleRate * channels * bps/8       */
    uint16_t blockAlign;        /* channels * bps/8                    */
    uint16_t bitsPerSample;     /* 16                                  */
    char     data[4];           /* "data"                              */
    uint32_t subchunk2Size;     /* number of PCM data bytes            */
} WavHeader_t;

/* ── SDMMC1 peripheral handle ─────────────────────────────────────────────── */
static SD_HandleTypeDef s_hsd1;

/* ── FatFS objects ─────────────────────────────────────────────────────────── */
static FATFS   s_fatfs;             /* FatFS work area for drive "0:"           */
static FIL     s_wavFile;           /* open WAV file handle                     */

/* ── Recording state ─────────────────────────────────────────────────────── */
static uint32_t  s_dataBytesWritten = 0;   /* PCM bytes written so far           */
static char      s_currentFilename[16];    /* e.g. "REC_001.wav\0"               */
static bool      s_sdReady          = false;
static bool      s_fileOpen         = false;
static volatile bool s_sdBusy       = false;  /* true while writing or streaming — callers must avoid concurrent FatFS access */

/* ── Double-buffer write pool (Ping-Pong) ────────────────────────────────────
 * Two 4 KB sector-aligned buffers.  AudioSD_WriteFrame() fills s_wbuf[s_wFill]
 * sample-by-sample; when it reaches WBUF_BYTES it calls f_write on that buffer
 * and switches to the other one.  Writing always happens in full 4 KB units →
 * no SD-card Read-Modify-Write → no bus stall → no audio glitch.
 *
 * Placement: AXI SRAM (.sram_bss) — accessible by SDMMC IDMA.
 * 32-byte alignment: required for D-Cache clean/invalidate on H7.
 * WBUF_BYTES must be a multiple of (AUDIO_SD_SAMPLES_PER_FRAME × 2):
 *   4096 / (512 × 2) = 4 frames per buffer — guaranteed no split.
 *
 * D-Cache safety: s_wbuf is in AXI SRAM which is M7 D-Cache cacheable.
 * s_wbuf is NEVER passed directly to SDMMC IDMA.  It flows:
 *   s_wbuf → f_write() → FatFS → disk_write() → s_sectorBuf (with SCB_CleanDCache).
 * The D-Cache flush is performed on s_sectorBuf in disk_write(), not here.
 * If the data path ever changes to bypass FatFS, add SCB_CleanDCache_by_Addr()
 * on s_wbuf before any direct DMA operation.
 * ─────────────────────────────────────────────────────────────────────────── */
#define WBUF_BYTES    4096u
#define WBUF_SAMPLES  (WBUF_BYTES / sizeof(int16_t))   /* 2048 int16 per buffer */

static int16_t  s_wbuf[2][WBUF_SAMPLES] __attribute__((aligned(32)));
static uint32_t s_wbufFill    = 0u;   /* bytes used in s_wbuf[s_wFillIdx] */
static uint8_t  s_wFillIdx    = 0u;   /* which buffer is being filled (0 or 1) */

/* SDMMC IDMA sector buffer — must be 4-byte aligned, in DMA-accessible RAM.
 * Used by the diskio callbacks below. */
static uint8_t s_sectorBuf[512u] __attribute__((aligned(32)));

/* huart8 owned by main.c / ble_uart.c */
extern UART_HandleTypeDef huart8;

/* Set by AudioSD_NotifyReady() (called from routeAsciiMessage) when NORA
 * sends "AUDIO:READY". Polled by AudioSD_SendFileToUART(). */
static volatile bool s_audioReady = false;

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void     SDMMC1_GPIO_Init(void);
static void     SDMMC1_Peripheral_Init(void);
static void     BuildWavHeader(WavHeader_t *hdr, uint32_t dataBytes);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* ── AudioSD_Init ─────────────────────────────────────────────────────────────
 * Initialise SDMMC1 and mount FatFS volume "0:".
 * Returns false if the card is absent or the mount fails; in that case, the
 * SD recording path is silently disabled — UART streaming still works.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_Init(void)
{
    STAGE("SDINIT1 PASS");
    SDMMC1_GPIO_Init();

    /* Check card is inserted BEFORE initialising the peripheral.
     * SD_DETECT = PI8, active-low: GPIO_PIN_SET means card absent.
     * If we call SDMMC1_Peripheral_Init() with no card, HAL_SD_Init()
     * fails and calls Error_Handler() — crashing the entire system. */
    if (HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_8) == GPIO_PIN_SET)
    {
        STAGE("SDINIT2 FAIL");
        SD_LOG("ABSENT");
        return false;
    }
    STAGE("SDINIT2 PASS");

    SDMMC1_Peripheral_Init();
    STAGE("SDINIT3 PASS");

    /* ── TEMPORARY: sector-0 diagnostic (ITM Terminal I/O only) ─────────────
     * Expected output in IAR Terminal I/O for a good MBR+exFAT card:
     *   sec0 dr=0
     *   sec0 sig=43605     ← 0xAA55 in decimal
     *   sec0 type=7        ← 0x07 = exFAT/NTFS partition type
     *   sec0 lba=2048      ← partition start sector
     *   sec0 oemid=[EXFAT   ]  ← only if sector 0 IS the VBR (SFD format)
     * ─────────────────────────────────────────────────────────────────────── */
    {
        DRESULT dr = disk_read(0, s_sectorBuf, 0, 1);
        _itm_str("sec0 dr=");
        _itm_u32((uint32_t)dr);
        _itm_str("sec0 sig=");
        _itm_u32(((uint32_t)s_sectorBuf[511] << 8) | s_sectorBuf[510]);
        _itm_str("sec0 type=");
        _itm_u32(s_sectorBuf[0x1C2]);
        _itm_str("sec0 lba=");
        _itm_u32(((uint32_t)s_sectorBuf[0x1C9] << 24) |
                 ((uint32_t)s_sectorBuf[0x1C8] << 16) |
                 ((uint32_t)s_sectorBuf[0x1C7] <<  8) |
                  (uint32_t)s_sectorBuf[0x1C6]);
        _itm_str("sec0 oemid=[");
        for (int _i = 3; _i < 11; _i++) {
            uint8_t _c = s_sectorBuf[_i];
            ITM_SendChar((_c >= 0x20u && _c < 0x7Fu) ? _c : '?');
        }
        _itm_str("]\n");
    }

    /* Register filesystem object — deferred mount (opt=0) per UM1722 / FatFS docs.
     * disk_initialize + BPB read are deferred until the first file operation
     * (f_open in SDWriteTask).  f_mount(opt=0) always returns FR_OK here. */
    f_mount(&s_fatfs, "0:", 0);
    STAGE("SDINIT4 PASS");

    s_sdReady = true;
    SD_LOG("OK");
    return true;
}

/* ── AudioSD_StartRecording ───────────────────────────────────────────────────
 * Choose the next REC_NNN.wav filename, open the file for writing,
 * and write a placeholder 44-byte WAV header.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_StartRecording(char *filename_out, uint8_t maxLen)
{
    if (!s_sdReady)
    {
        SD_LOG("NOT_READY");
        return false;
    }

    /* Find the next unused REC_NNN.wav (max AUDIO_SD_MAX_REC_NUM attempts) */
    FRESULT res = FR_EXIST;
    for (uint16_t n = 1; n <= AUDIO_SD_MAX_REC_NUM; n++)
    {
        snprintf(s_currentFilename, sizeof(s_currentFilename), "REC_%03u.wav", n);
        FILINFO fno;
        res = f_stat(s_currentFilename, &fno); /* FR_NO_FILE = name is free */
        if (res == FR_NO_FILE)
        {
            break;
        }
    }

    if (res != FR_NO_FILE)
    {
        SD_LOG("NO_FREE_NAME");
        return false;
    }

    /* Open the new file for writing */
    res = f_open(&s_wavFile, s_currentFilename, FA_WRITE | FA_CREATE_NEW);
    if (res != FR_OK)
    {
        SD_LOG("OPEN_FAIL:%d — attempting remount", (int)res);
        /* FatFS internal state may be stale after USB MSC raw access.
         * Force unmount + remount to recover the volume. */
        f_mount(NULL, "0:", 0);
        FRESULT fr2 = f_mount(&s_fatfs, "0:", 1);
        if (fr2 != FR_OK)
        {
            SD_LOG("REMOUNT_FAIL:%d", (int)fr2);
            s_sdReady = false;
            return false;
        }
        SD_LOG("REMOUNT_OK — retrying f_open");
        res = f_open(&s_wavFile, s_currentFilename, FA_WRITE | FA_CREATE_NEW);
        if (res != FR_OK)
        {
            SD_LOG("OPEN_FAIL_AFTER_REMOUNT:%d", (int)res);
            s_sdReady = false;
            return false;
        }
    }

    /* Write placeholder header — will be overwritten on stop with real sizes */
    WavHeader_t hdr;
    BuildWavHeader(&hdr, 0 /* placeholder */);
    UINT bw;
    f_write(&s_wavFile, &hdr, sizeof(hdr), &bw);

    s_dataBytesWritten = 0;
    s_wbufFill         = 0u;   /* reset ping-pong state for new recording */
    s_wFillIdx         = 0u;
    s_fileOpen         = true;

    if (filename_out && maxLen > 0)
    {
        snprintf(filename_out, maxLen, "%s", s_currentFilename);
    }

    SD_LOG("REC_START:%s", s_currentFilename);
    return true;
}

/* ── AudioSD_WriteFrame ───────────────────────────────────────────────────────
 * Convert nSamples int32 DFSDM words to int16 PCM and accumulate in the
 * ping-pong write buffer.  f_write is only called when a full WBUF_BYTES (4 KB)
 * sector is ready — eliminating SD card Read-Modify-Write and the bus stalls
 * that caused audio glitches in the previous 1 KB per-frame write approach.
 *
 * WBUF_BYTES (4096) is a multiple of (AUDIO_SD_SAMPLES_PER_FRAME × 2 = 1024),
 * so a frame will never split across buffer boundaries.
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioSD_WriteFrame(const int32_t *src32, uint32_t nSamples)
{
    if (!s_fileOpen) return;

    /* Convert int32 DFSDM → int16 PCM directly into the fill buffer */
    int16_t *dst = &s_wbuf[s_wFillIdx][s_wbufFill / sizeof(int16_t)];
    for (uint32_t i = 0; i < nSamples; i++)
        dst[i] = (int16_t)(src32[i] >> 8);

    s_wbufFill += nSamples * sizeof(int16_t);   /* += 1024 bytes */

    /* Full 4 KB sector ready — write it, then ping-pong to the other buffer */
    if (s_wbufFill >= WBUF_BYTES)
    {
        UINT    bw;
        FRESULT res = f_write(&s_wavFile, s_wbuf[s_wFillIdx], WBUF_BYTES, &bw);
        if (res == FR_OK && bw == WBUF_BYTES)
            s_dataBytesWritten += WBUF_BYTES;
        else
            SD_LOG("WRITE_ERR buf=%u", s_wFillIdx);

        s_wFillIdx  ^= 1u;   /* 0 → 1 → 0 → ... */
        s_wbufFill   = 0u;
    }
}

/* ── AudioSD_StopRecording ────────────────────────────────────────────────────
 * Patch the WAV header with the real data size, flush and close the file.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_StopRecording(char *filename_out, uint8_t maxLen)
{
    if (!s_fileOpen)
    {
        SD_LOG("NO_FILE_OPEN");
        return false;
    }

    /* Flush remaining PCM in write buffer (0..3 partial frames at stop time) */
    if (s_wbufFill > 0u)
    {
        UINT bw;
        FRESULT fres = f_write(&s_wavFile, s_wbuf[s_wFillIdx], s_wbufFill, &bw);
        if (fres == FR_OK && bw == s_wbufFill)
            s_dataBytesWritten += s_wbufFill;
        else
            SD_LOG("FLUSH_ERR remaining=%lu", (unsigned long)s_wbufFill);
        s_wbufFill = 0u;
    }

    /* Seek back to byte 0 and overwrite the placeholder header */
    FRESULT res = f_lseek(&s_wavFile, 0);
    if (res != FR_OK)
    {
        SD_LOG("SEEK_FAIL");
        f_close(&s_wavFile);
        s_fileOpen = false;
        return false;
    }

    WavHeader_t hdr;
    BuildWavHeader(&hdr, s_dataBytesWritten);
    UINT bw;
    f_write(&s_wavFile, &hdr, sizeof(hdr), &bw);

    f_close(&s_wavFile);  /* f_close flushes + finalises directory entry — f_sync before close is redundant */
    s_fileOpen = false;

    if (filename_out && maxLen > 0)
    {
        snprintf(filename_out, maxLen, "%s", s_currentFilename);
    }

    SD_LOG("REC_STOP:%s %lu bytes", s_currentFilename, (unsigned long)s_dataBytesWritten);
    return true;
}

/* ── AudioSD_SendFileToUART ──────────────────────────────────────────────────
 * Stream the WAV file to NORA using the protocol:
 *   "AUDIO:FILE:REC_001.wav:<size>\n"   ASCII header
 *   <size raw bytes>                    binary WAV payload
 *
 * Reads in AUDIO_SD_UART_CHUNK-byte blocks to keep stack usage bounded.
 * Returns true if all bytes were transmitted.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_SendFileToUART(const char *filename)
{
    if (!s_sdReady)
    {
        SD_LOG("NOT_READY");
        return false;
    }

    s_sdBusy = true;

    /* Remount before reading — the write session leaves the SDMMC DPSM in a
     * degraded state (RX overrun) that causes FR_DISK_ERR mid-read without this. */
    AudioSD_Remount();

    /* Get file size before opening for reading — need it for the UART header */
    FILINFO fno;
    if (f_stat(filename, &fno) != FR_OK)
    {
        SD_LOG("FSTAT_FAIL:%s", filename);
        s_sdBusy = false;
        return false;
    }
    FSIZE_t fileSize = fno.fsize;

    /* Flush NORA's line buffer before sending the header.
     * STM32 UART TX glitches during reset leave garbage bytes in NORA's
     * line[] accumulation buffer (pos > 0).  Two bare newlines reset pos=0
     * so the AUDIO:FILE: line is matched at position 0 (B-007). */
    const uint8_t flush[] = "\n\n";
    HAL_UART_Transmit(&huart8, (uint8_t *)flush, 2, 100);
    vTaskDelay(pdMS_TO_TICKS(50));   /* give NORA time to process the flushes */

    /* Send ASCII header so NORA knows filename and exact byte count */
    char hdrMsg[48];
    int  hdrLen = snprintf(hdrMsg, sizeof(hdrMsg),
                           "AUDIO:FILE:%s:%lu\n",
                           filename, (unsigned long)fileSize);
    HAL_UART_Transmit(&huart8, (uint8_t *)hdrMsg, (uint16_t)hdrLen, 200);

    /* Clear any stale AUDIO:READY from a previous session before sending
     * the header — prevents a leftover flag from triggering an early stream. */
    s_audioReady = false;

    /* Wait for NORA to send "AUDIO:READY" — signals TLS done + GCS HTTP PUT open.
     * Replaces the old fixed 1 s delay: the flag is set by AudioSD_NotifyReady()
     * which routeAsciiMessage() calls the moment AUDIO:READY arrives on UART8.
     * Timeout: 10 s (TLS typically < 1 s; 10 s covers slow WiFi). */
    SD_LOG("Waiting for AUDIO:READY from NORA...");
    uint32_t t0 = HAL_GetTick();
    while (!s_audioReady)
    {
        if ((HAL_GetTick() - t0) >= 10000u)
        {
            SD_LOG("AUDIO:READY timeout — NORA not ready");
            s_sdBusy = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    SD_LOG("AUDIO:READY received — streaming now");

    /* Open file AFTER the 2s delay — keeps f_open fresh immediately before
     * reading.  Holding the file open during vTaskDelay allows other tasks
     * to disturb the FatFS win[] sector cache → FR_DISK_ERR mid-read. */
    static FIL file __attribute__((aligned(32)));
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK)
    {
        SD_LOG("FILE_OPEN_FAIL:%d", (int)res);
        s_sdBusy = false;
        return false;
    }

    /* Stream file in chunks */
    static uint8_t s_txChunk[AUDIO_SD_UART_CHUNK] __attribute__((aligned(32)));
    UINT     br;
    bool     ok       = true;
    uint32_t sentTotal = 0;

    while (1)
    {
        res = f_read(&file, s_txChunk, sizeof(s_txChunk), &br);
        if (res != FR_OK)
        {
            SD_LOG("FREAD_ERR:%d at %lu sdErr=0x%08lX", (int)res, sentTotal,
                   (unsigned long)s_hsd1.ErrorCode);
            ok = false;
            break;
        }
        if (br == 0) break;   /* clean EOF */

        HAL_StatusTypeDef txRes =
            HAL_UART_Transmit(&huart8, s_txChunk, (uint16_t)br, 500);
        if (txRes != HAL_OK)
        {
            SD_LOG("UART_TX_ERR:%d at %lu", (int)txRes, sentTotal);
            ok = false;
            break;
        }
        sentTotal += br;

        /* Pace TX: NORA's UART buffer is 16 KB; STM32 sends 96 KB at ~115 KB/s.
         * Without a delay the buffer overflows after ~140 ms and bytes are
         * silently dropped by the ESP32 UART driver.
         * 10 ms per 1 KB chunk = ~10 KB/s effective rate — well within NORA's
         * ability to drain (reads 1 KB, writes to GCS HTTP, loops). */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    SD_LOG("Stream done: %lu/%lu bytes ok=%d", sentTotal, (unsigned long)fileSize, (int)ok);

    f_close(&file);

    /* After reading back the file, the SDMMC peripheral may have accumulated
     * an error (e.g. RX overrun from write→read transition).  Recover now so
     * the next recording starts from a clean state. */
    AudioSD_Remount();

    s_sdBusy = false;
    return ok;
}

/* ── AudioSD_SDMMC_IRQHandler ─────────────────────────────────────────────── */
void AudioSD_SDMMC_IRQHandler(void)
{
    HAL_SD_IRQHandler(&s_hsd1);
}

/* ── AudioSD_GetErrorCode ─────────────────────────────────────────────────── */
uint32_t AudioSD_GetErrorCode(void)
{
    return s_hsd1.ErrorCode;
}

/* ── AudioSD_IsBusy ───────────────────────────────────────────────────────── */
bool AudioSD_IsBusy(void)
{
    return s_sdBusy;
}

/* ── AudioSD_NotifyReady ──────────────────────────────────────────────────────
 * Called by routeAsciiMessage() in main.c when "AUDIO:READY" arrives from NORA.
 * Sets the flag that AudioSD_SendFileToUART() polls while waiting for the ACK.
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioSD_NotifyReady(void)
{
    s_audioReady = true;
}

/* ── AudioSD_Remount ──────────────────────────────────────────────────────────
 * Recover the SDMMC peripheral and re-mount FatFS.
 *
 * After a write→read transition the SDMMC DPSM can accumulate an RX overrun
 * error (SDMMC_ERROR_RX_OVERRUN = 0x20) that prevents HAL_SD_GetCardState()
 * from returning HAL_SD_CARD_TRANSFER.  disk_status() then returns STA_NOINIT
 * and every subsequent FatFS call fails with FR_NOT_READY.
 *
 * Recovery sequence:
 *   1. HAL_SD_Abort  — abort any pending SDMMC transfer, reset DPSM
 *   2. Clear ErrorCode — erase accumulated error flags
 *   3. f_mount(NULL) — force FatFS to release the volume
 *   4. f_mount(1)    — re-mount; calls disk_initialize which polls GetCardState
 *
 * Returns true if the volume is usable again.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_Remount(void)
{
    STAGE("REMOUNT1 PASS");
    /* Soft abort first — clears SDMMC interrupt flags and HAL state */
    HAL_SD_Abort(&s_hsd1);
    s_hsd1.ErrorCode = HAL_SD_ERROR_NONE;

    /* Full peripheral de-init + re-init.
     * HAL_SD_Abort alone resets the HAL state machine but does NOT reset the
     * SDMMC IDMA or the hardware data-path state machine (DPSM).  After an
     * RX overrun the DPSM stays stuck and HAL_SD_ReadBlocks keeps failing.
     * HAL_SD_DeInit powers off SDMMC1; SDMMC1_Peripheral_Init re-runs the
     * full card-detect handshake (CMD0/CMD8/ACMD41) so ReadBlocks works again. */
    HAL_SD_DeInit(&s_hsd1);
    vTaskDelay(pdMS_TO_TICKS(20u));   /* let card power-cycle settle */
    SDMMC1_Peripheral_Init();         /* full re-init: HAL_SD_Init + 4-bit bus */
    STAGE("REMOUNT2 PASS");

    f_mount(NULL, "0:", 0);                        /* force unmount */
    FRESULT fr = f_mount(&s_fatfs, "0:", 1);       /* re-mount now */
    s_sdReady = (fr == FR_OK);
    SD_LOG("Remount result: fr=%d sdReady=%d", (int)fr, (int)s_sdReady);
    if (s_sdReady) STAGE("REMOUNT3 PASS");
    else           STAGE("REMOUNT3 FAIL");
    return s_sdReady;
}

/* ── AudioSD_DeInit ───────────────────────────────────────────────────────────
 * Release SDMMC1 and unmount FatFS — called before USB MSC takes ownership.
 * Does NOT re-initialize. After this, the caller owns SDMMC1.
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioSD_DeInit(void)
{
    f_mount(NULL, "0:", 0);     /* unmount — FatFS releases volume state */
    HAL_SD_Abort(&s_hsd1);
    HAL_SD_DeInit(&s_hsd1);
    s_sdReady = false;
    RLOG("[SD] AudioSD_DeInit — SDMMC1 released for USB MSC");
}

/* ── AudioSD_Format ───────────────────────────────────────────────────────────
 * Format the SD card as exFAT and immediately mount the new volume.
 * Called from SDWriteTask when f_open returns FR_NO_FILESYSTEM — meaning the
 * card is blank or has an unrecognised format.
 * Returns true if the volume is mounted and ready for file operations.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_Format(void)
{
    SD_LOG("FORMAT — card has no filesystem, formatting as exFAT...");
    /* work[] must be >= cluster size (FR_NOT_ENOUGH_CORE if too small).
     * FF_MULTI_PARTITION=1: f_mkfs creates MBR at sector 0 + exFAT partition.
     * f_mount then reads MBR → finds partition 1 → mounts. */
    static uint8_t work[4096u];
    static const MKFS_PARM opt = { FM_EXFAT, 0, 0, 0, 4096u };
    STAGE("FMT1 PASS");
    FRESULT res = f_mkfs("0:", &opt, work, sizeof(work));
    if (res != FR_OK) {
        STAGE("FMT2 FAIL");
        SD_LOG("FORMAT_FAIL:%d", (int)res);
        return false;
    }
    STAGE("FMT2 PASS");

    res = f_mount(&s_fatfs, "0:", 1);
    if (res != FR_OK) {
        STAGE("FMT3 FAIL");
        SD_LOG("FORMAT_REMOUNT_FAIL:%d", (int)res);
        return false;
    }
    STAGE("FMT3 PASS");
    s_sdReady = true;
    SD_LOG("FORMAT+MOUNT OK");
    return true;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Private helpers                                                             */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* ── BuildWavHeader ──────────────────────────────────────────────────────── */
static void BuildWavHeader(WavHeader_t *hdr, uint32_t dataBytes)
{
    memcpy(hdr->riff, "RIFF", 4);
    hdr->chunkSize     = 36u + dataBytes;          /* RIFF chunk size = 36 + data */
    memcpy(hdr->wave, "WAVE", 4);
    memcpy(hdr->fmt,  "fmt ", 4);
    hdr->subchunk1Size = 16u;                      /* PCM fmt chunk is always 16  */
    hdr->audioFormat   = 1u;                       /* 1 = linear PCM              */
    hdr->numChannels   = AUDIO_SD_CHANNELS;
    hdr->sampleRate    = AUDIO_SD_SAMPLE_RATE;
    hdr->byteRate      = AUDIO_SD_SAMPLE_RATE
                         * AUDIO_SD_CHANNELS
                         * (AUDIO_SD_BITS_PER_SAMPLE / 8u);
    hdr->blockAlign    = (uint16_t)(AUDIO_SD_CHANNELS
                         * (AUDIO_SD_BITS_PER_SAMPLE / 8u));
    hdr->bitsPerSample = AUDIO_SD_BITS_PER_SAMPLE;
    memcpy(hdr->data,  "data", 4);
    hdr->subchunk2Size = dataBytes;
}

/* ── SDMMC1_GPIO_Init ────────────────────────────────────────────────────── */
static void SDMMC1_GPIO_Init(void)
{
    /* Enable GPIO clocks */
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    /* PC8..PC12 : SDMMC1_D0..D3, SDMMC1_CK */
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10
                   | GPIO_PIN_11 | GPIO_PIN_12;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF12_SDIO1;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* PD2 : SDMMC1_CMD */
    gpio.Pin       = GPIO_PIN_2;
    gpio.Alternate = GPIO_AF12_SDIO1;
    HAL_GPIO_Init(GPIOD, &gpio);

    /* PI8 : SD_DETECT (active-low card-detect, input with pull-up) */
    gpio.Pin  = GPIO_PIN_8;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOI, &gpio);
}

/* ── SDMMC1_Peripheral_Init ──────────────────────────────────────────────── */
static void SDMMC1_Peripheral_Init(void)
{
    __HAL_RCC_SDMMC1_CLK_ENABLE();

    s_hsd1.Instance = SDMMC1;
    /* ClockDiv=4: SDMMC clock = 200 MHz / (2*4) = 25 MHz.
     * ClockDiv=2 gives 50 MHz which causes RXOVERR (0x20) under LTDC AXI load —
     * LTDC DMA bursts starve the SDMMC IDMA, FIFO overflows mid-read.
     * 25 MHz doubles the FIFO fill time, eliminating contention. */
    s_hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    s_hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    s_hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;   /* 1-bit during init */
    s_hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
    s_hsd1.Init.ClockDiv            = 4;

    if (HAL_SD_Init(&s_hsd1) != HAL_OK)
    {
        /* Non-fatal — SD recording unavailable but system keeps running */
        SD_LOG("INIT_FAIL");
        return;
    }

    /* Switch to 4-bit wide bus for higher throughput */
    if (HAL_SD_ConfigWideBusOperation(&s_hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
    {
        /* Some cards don't support 4-bit; continue with 1-bit */
        SD_LOG("1BIT_MODE");
    }

    HAL_NVIC_SetPriority(SDMMC1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(SDMMC1_IRQn);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  FatFS disk I/O callbacks (diskio.c interface)                               */
/*  Drive number 0 = SDMMC1.                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (HAL_SD_GetCardState(&s_hsd1) == HAL_SD_CARD_TRANSFER) return 0;
    return STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    if (HAL_SD_GetCardState(&s_hsd1) == HAL_SD_CARD_TRANSFER) return 0;
    return STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0) return RES_PARERR;

    /* Wait for card to reach TRANSFER state before issuing a new command.
     * After DFSDM recording stops, SDMMC may still be finishing a previous
     * operation. HAL_SD_ReadBlocks returns HAL_ERROR if card is not READY. */
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER)
    {
        if ((HAL_GetTick() - t0) > 500u) return RES_ERROR;
    }

    /* Always use 32-byte aligned bounce buffer — never DMA directly to caller's buff.
     * B-005/B-006: post-DMA SCB_InvalidateDCache_by_Addr rounds addr down to 32-byte
     * boundary, hitting adjacent FATFS struct fields (winsect before win[]) and
     * discarding their dirty cache lines → FR_INT_ERR / directory corruption.
     * s_sectorBuf is static + 32-byte aligned: no adjacent fields at risk. */
    for (UINT i = 0; i < count; i++)
    {
        /* Disable task preemption during SD read — prevents SDMMC IDMA FIFO
         * overrun caused by other tasks accessing AHB3 (SDRAM FMC refresh)
         * mid-transfer. vTaskSuspendAll/ResumeAll keeps interrupts active
         * (UART DMA, SysTick still work) but prevents task switches. */
        vTaskSuspendAll();
        HAL_StatusTypeDef rdResult = HAL_SD_ReadBlocks(&s_hsd1, s_sectorBuf,
                              (uint32_t)(sector + i), 1, 1000);
        xTaskResumeAll();

        if (rdResult != HAL_OK)
        {
            /* RXOVERR (0x20) / TX_UNDERRUN (0x10): host-side FIFO error.
             * The SD card is fine and returns to TRANSFER state on its own.
             * DO NOT DeInit + re-init — HAL_SD_Init re-runs CMD0/ACMD41 which
             * takes 2–5 s on this card and causes a streaming timeout at NORA.
             *
             * Lightweight host-only reset:
             *   1. HAL_SD_Abort  — stops IDMA, clears HAL state machine
             *   2. SDMMC1->DCTRL = 0 — disable DPSM (data path state machine)
             *   3. SDMMC1->ICR   = 0x1FE007FF — clear all status flags
             *   4. Clear ErrorCode — erase accumulated flags
             *   5. Poll card back to TRANSFER — typically < 5 ms
             *   6. Retry the read once.
             */
            SD_LOG("disk_read FAIL sec=%lu sdErr=0x%08lX — DPSM reset+retry",
                   (unsigned long)(sector + i), (unsigned long)s_hsd1.ErrorCode);
            HAL_SD_Abort(&s_hsd1);
            SDMMC1->DCTRL = 0;
            SDMMC1->ICR   = 0x1FE007FFu;
            s_hsd1.ErrorCode = HAL_SD_ERROR_NONE;
            /* Wait for card to return to TRANSFER state — usually immediate */
            {
                uint32_t _tr = HAL_GetTick();
                while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER)
                {
                    if ((HAL_GetTick() - _tr) > 500u)
                    {
                        SD_LOG("disk_read card-state timeout after reset sec=%lu",
                               (unsigned long)(sector + i));
                        return RES_ERROR;
                    }
                }
            }
            SD_LOG("disk_read retry sec=%lu", (unsigned long)(sector + i));
            vTaskSuspendAll();
            HAL_StatusTypeDef retryResult = HAL_SD_ReadBlocks(&s_hsd1, s_sectorBuf,
                                  (uint32_t)(sector + i), 1, 1000);
            xTaskResumeAll();
            if (retryResult != HAL_OK)
            {
                SD_LOG("disk_read retry FAIL sec=%lu sdErr=0x%08lX — full remount",
                       (unsigned long)(sector + i), (unsigned long)s_hsd1.ErrorCode);
                if (!AudioSD_Remount())
                {
                    SD_LOG("disk_read remount FAIL — SD unrecoverable");
                    return RES_ERROR;
                }
                vTaskSuspendAll();
                HAL_StatusTypeDef finalResult = HAL_SD_ReadBlocks(&s_hsd1, s_sectorBuf,
                                      (uint32_t)(sector + i), 1, 1000);
                xTaskResumeAll();
                if (finalResult != HAL_OK)
                {
                    SD_LOG("disk_read final FAIL sec=%lu sdErr=0x%08lX",
                           (unsigned long)(sector + i), (unsigned long)s_hsd1.ErrorCode);
                    return RES_ERROR;
                }
            }
        }
        {
            uint32_t _t0 = HAL_GetTick();
            while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER)
            {
                if (HAL_GetTick() - _t0 > 500u)
                {
                    SD_LOG("disk_read card state timeout sec=%lu",
                           (unsigned long)(sector + i));
                    return RES_ERROR;
                }
            }
        }
        /* HAL_SD_ReadBlocks uses CPU FIFO polling (not IDMA/DMA) — data is written
         * directly to s_sectorBuf via tempbuff pointer, leaving dirty cache lines.
         * SCB_InvalidateDCache_by_Addr (DCIMVAC) would DISCARD those dirty lines
         * before they are flushed to SRAM → memcpy would read stale zeros from SRAM.
         * No cache operation needed here: the CPU itself wrote the data; it is
         * already in the cache and the memcpy reads it directly from there. */
        memcpy(buff + i * 512u, s_sectorBuf, 512u);
    }
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
    if (pdrv != 0) return RES_PARERR;

    /* Wait for card TRANSFER state before writing */
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER)
    {
        if ((HAL_GetTick() - t0) > 500u) return RES_ERROR;
    }

    /* Always use bounce buffer — same reasoning as disk_read above. */
    for (UINT i = 0; i < count; i++)
    {
        memcpy(s_sectorBuf, buff + i * 512u, 512u);
        SCB_CleanDCache_by_Addr((uint32_t *)s_sectorBuf, 512);

        /* Disable task preemption during SD write — same AHB3 bus contention
         * hazard as disk_read: SDRAM FMC refresh can stall SDMMC IDMA FIFO.
         * Interrupts remain active (UART DMA, SysTick unaffected). */
        vTaskSuspendAll();
        HAL_StatusTypeDef wrResult = HAL_SD_WriteBlocks(&s_hsd1, s_sectorBuf,
                                         (uint32_t)(sector + i), 1, 1000);
        xTaskResumeAll();

        if (wrResult != HAL_OK)
        {
            /* TX_UNDERRUN (0x10) / RXOVERR (0x20): host-side FIFO error.
             * Same lightweight reset as disk_read — no DeInit to avoid 2–5 s hang. */
            SD_LOG("disk_write FAIL sec=%lu sdErr=0x%08lX — DPSM reset+retry",
                   (unsigned long)(sector + i), (unsigned long)s_hsd1.ErrorCode);
            HAL_SD_Abort(&s_hsd1);
            SDMMC1->DCTRL = 0;
            SDMMC1->ICR   = 0x1FE007FFu;
            s_hsd1.ErrorCode = HAL_SD_ERROR_NONE;
            {
                uint32_t _tr = HAL_GetTick();
                while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER)
                {
                    if ((HAL_GetTick() - _tr) > 500u)
                    {
                        SD_LOG("disk_write card-state timeout after reset sec=%lu",
                               (unsigned long)(sector + i));
                        return RES_ERROR;
                    }
                }
            }
            /* Re-clean cache line before retry — the bounce buffer is still valid */
            SCB_CleanDCache_by_Addr((uint32_t *)s_sectorBuf, 512);
            SD_LOG("disk_write retry sec=%lu", (unsigned long)(sector + i));
            vTaskSuspendAll();
            HAL_StatusTypeDef retryResult = HAL_SD_WriteBlocks(&s_hsd1, s_sectorBuf,
                                                 (uint32_t)(sector + i), 1, 1000);
            xTaskResumeAll();
            if (retryResult != HAL_OK)
            {
                SD_LOG("disk_write retry FAIL sec=%lu sdErr=0x%08lX — full remount",
                       (unsigned long)(sector + i), (unsigned long)s_hsd1.ErrorCode);
                /* Lightweight DPSM reset insufficient — HAL state machine still stuck.
                 * Full DeInit+reinit as last resort (costs ~200ms but recovers correctly). */
                if (!AudioSD_Remount())
                {
                    SD_LOG("disk_write remount FAIL — SD unrecoverable");
                    return RES_ERROR;
                }
                /* Retry once more after full remount */
                SCB_CleanDCache_by_Addr((uint32_t *)s_sectorBuf, 512);
                vTaskSuspendAll();
                HAL_StatusTypeDef finalResult = HAL_SD_WriteBlocks(&s_hsd1, s_sectorBuf,
                                                     (uint32_t)(sector + i), 1, 1000);
                xTaskResumeAll();
                if (finalResult != HAL_OK)
                {
                    SD_LOG("disk_write final FAIL sec=%lu sdErr=0x%08lX",
                           (unsigned long)(sector + i), (unsigned long)s_hsd1.ErrorCode);
                    return RES_ERROR;
                }
            }
        }
        {
            uint32_t _t0 = HAL_GetTick();
            while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER)
            {
                if (HAL_GetTick() - _t0 > 500u)
                {
                    SD_LOG("disk_write card state timeout sec=%lu",
                           (unsigned long)(sector + i));
                    return RES_ERROR;
                }
            }
        }
    }
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0) return RES_PARERR;

    HAL_SD_CardInfoTypeDef info;
    switch (cmd)
    {
        case CTRL_SYNC:
            /* Nothing to flush — HAL_SD_WriteBlocks is synchronous */
            return RES_OK;

        case GET_SECTOR_COUNT:
            HAL_SD_GetCardInfo(&s_hsd1, &info);
            *(DWORD *)buff = info.LogBlockNbr;
            return RES_OK;

        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;

        case GET_BLOCK_SIZE:
            HAL_SD_GetCardInfo(&s_hsd1, &info);
            *(DWORD *)buff = info.LogBlockSize / 512u;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}

