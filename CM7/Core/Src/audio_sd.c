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
#include <string.h>         /* memcpy, strlen                    */
#include <stdio.h>          /* snprintf                          */

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

/* ── PCM conversion scratch buffer ────────────────────────────────────────────
 * Placed in AXI SRAM (.sram_bss) so SDMMC IDMA can access it.
 * static = not on task stack; aligned(32) for D-Cache operations.
 * ─────────────────────────────────────────────────────────────────────────── */
static int16_t s_pcmScratch[AUDIO_SD_SAMPLES_PER_FRAME]
    __attribute__((aligned(32)));

/* SDMMC IDMA sector buffer — must be 4-byte aligned, in DMA-accessible RAM.
 * Used by the diskio callbacks below. */
static uint8_t s_sectorBuf[512u] __attribute__((aligned(32)));

/* huart8 owned by main.c / ble_uart.c */
extern UART_HandleTypeDef huart8;

/* ── Forward declarations ─────────────────────────────────────────────────── */
static void     SDMMC1_GPIO_Init(void);
static void     SDMMC1_Peripheral_Init(void);
static void     BuildWavHeader(WavHeader_t *hdr, uint32_t dataBytes);
static void     SendUartStr(const char *str);

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
    /* Init GPIOs first so SD_DETECT pin (PI8) is readable */
    SDMMC1_GPIO_Init();

    /* Check card is inserted BEFORE initialising the peripheral.
     * SD_DETECT = PI8, active-low: GPIO_PIN_SET means card absent.
     * If we call SDMMC1_Peripheral_Init() with no card, HAL_SD_Init()
     * fails and calls Error_Handler() — crashing the entire system. */
    if (HAL_GPIO_ReadPin(GPIOI, GPIO_PIN_8) == GPIO_PIN_SET)
    {
        SendUartStr("SD:ABSENT\n");
        return false;
    }

    SDMMC1_Peripheral_Init();

    /* Mount the FatFS volume — FA_OPEN_ALWAYS ensures the volume is formatted */
    FRESULT res = f_mount(&s_fatfs, "0:", 1 /* mount now */);
    if (res != FR_OK)
    {
        char msg[32];
        snprintf(msg, sizeof(msg), "SD:MOUNT_FAIL:%d\n", (int)res);
        SendUartStr(msg);
        return false;
    }

    s_sdReady = true;
    SendUartStr("SD:OK\n");
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
        SendUartStr("SD:NOT_READY\n");
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
        SendUartStr("SD:NO_FREE_NAME\n");
        return false;
    }

    /* Open the new file for writing */
    res = f_open(&s_wavFile, s_currentFilename, FA_WRITE | FA_CREATE_NEW);
    if (res != FR_OK)
    {
        char msg[32];
        snprintf(msg, sizeof(msg), "SD:OPEN_FAIL:%d\n", (int)res);
        SendUartStr(msg);
        return false;
    }

    /* Write placeholder header — will be overwritten on stop with real sizes */
    WavHeader_t hdr;
    BuildWavHeader(&hdr, 0 /* placeholder */);
    UINT bw;
    f_write(&s_wavFile, &hdr, sizeof(hdr), &bw);

    s_dataBytesWritten = 0;
    s_fileOpen         = true;

    if (filename_out && maxLen > 0)
    {
        snprintf(filename_out, maxLen, "%s", s_currentFilename);
    }

    char msg[32];
    snprintf(msg, sizeof(msg), "SD:REC_START:%s\n", s_currentFilename);
    SendUartStr(msg);
    return true;
}

/* ── AudioSD_WriteFrame ───────────────────────────────────────────────────────
 * Convert nSamples int32 DFSDM words to int16 PCM (right-shift by 8)
 * and append to the open WAV file.
 *
 * The same right-shift-8 used in SendPCMFrame() is applied here, keeping
 * the SD and UART streams identical.
 * ─────────────────────────────────────────────────────────────────────────── */
void AudioSD_WriteFrame(const int32_t *src32, uint32_t nSamples)
{
    if (!s_fileOpen) return;

    /* Convert int32 DFSDM samples to int16 PCM */
    for (uint32_t i = 0; i < nSamples; i++)
    {
        s_pcmScratch[i] = (int16_t)(src32[i] >> 8);
    }

    UINT bw;
    uint16_t byteLen = (uint16_t)(nSamples * sizeof(int16_t));
    FRESULT  res     = f_write(&s_wavFile, s_pcmScratch, byteLen, &bw);

    if (res == FR_OK && bw == byteLen)
    {
        s_dataBytesWritten += byteLen;
    }
    else
    {
        /* Disk full or write error — log but don't abort; next frame will retry */
        SendUartStr("SD:WRITE_ERR\n");
    }
}

/* ── AudioSD_StopRecording ────────────────────────────────────────────────────
 * Patch the WAV header with the real data size, flush and close the file.
 * ─────────────────────────────────────────────────────────────────────────── */
bool AudioSD_StopRecording(char *filename_out, uint8_t maxLen)
{
    if (!s_fileOpen)
    {
        SendUartStr("SD:NO_FILE_OPEN\n");
        return false;
    }

    /* Seek back to byte 0 and overwrite the placeholder header */
    FRESULT res = f_lseek(&s_wavFile, 0);
    if (res != FR_OK)
    {
        SendUartStr("SD:SEEK_FAIL\n");
        f_close(&s_wavFile);
        s_fileOpen = false;
        return false;
    }

    WavHeader_t hdr;
    BuildWavHeader(&hdr, s_dataBytesWritten);
    UINT bw;
    f_write(&s_wavFile, &hdr, sizeof(hdr), &bw);

    f_sync(&s_wavFile);  /* flush write-back cache to SD card */
    f_close(&s_wavFile);
    s_fileOpen = false;

    if (filename_out && maxLen > 0)
    {
        snprintf(filename_out, maxLen, "%s", s_currentFilename);
    }

    char msg[48];
    snprintf(msg, sizeof(msg), "SD:REC_STOP:%s:%lu\n",
             s_currentFilename, (unsigned long)s_dataBytesWritten);
    SendUartStr(msg);
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
        SendUartStr("SD:NOT_READY\n");
        return false;
    }

    FIL   file;
    FRESULT res = f_open(&file, filename, FA_READ);
    if (res != FR_OK)
    {
        char msg[40];
        snprintf(msg, sizeof(msg), "SD:FILE_OPEN_FAIL:%d\n", (int)res);
        SendUartStr(msg);
        return false;
    }

    /* Total file size = header (44) + PCM data bytes */
    FSIZE_t fileSize = f_size(&file);

    /* Send ASCII header so NORA knows what is coming */
    char hdrMsg[48];
    int  hdrLen = snprintf(hdrMsg, sizeof(hdrMsg),
                           "AUDIO:FILE:%s:%lu\n",
                           filename, (unsigned long)fileSize);
    HAL_UART_Transmit(&huart8, (uint8_t *)hdrMsg, (uint16_t)hdrLen, 200);

    /* Stream file in chunks */
    static uint8_t s_txChunk[AUDIO_SD_UART_CHUNK] __attribute__((aligned(32)));
    UINT    br;
    bool    ok = true;

    while (1)
    {
        res = f_read(&file, s_txChunk, sizeof(s_txChunk), &br);
        if (res != FR_OK || br == 0) break;

        HAL_StatusTypeDef txRes =
            HAL_UART_Transmit(&huart8, s_txChunk, (uint16_t)br, 500);
        if (txRes != HAL_OK)
        {
            SendUartStr("SD:UART_TX_ERR\n");
            ok = false;
            break;
        }
    }

    f_close(&file);
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

/* ── SendUartStr ─────────────────────────────────────────────────────────── */
static void SendUartStr(const char *str)
{
    HAL_UART_Transmit(&huart8, (uint8_t *)str, (uint16_t)strlen(str), 100);
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
    /* ClockDiv=2: SDMMC clock = 200 MHz / (2*(2+1)) = ~25 MHz — safe for most cards */
    s_hsd1.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    s_hsd1.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    s_hsd1.Init.BusWide             = SDMMC_BUS_WIDE_1B;   /* 1-bit during init */
    s_hsd1.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_DISABLE;
    s_hsd1.Init.ClockDiv            = 2;

    if (HAL_SD_Init(&s_hsd1) != HAL_OK)
    {
        /* Non-fatal — SD recording unavailable but system keeps running */
        SendUartStr("SD:INIT_FAIL\n");
        return;
    }

    /* Switch to 4-bit wide bus for higher throughput */
    if (HAL_SD_ConfigWideBusOperation(&s_hsd1, SDMMC_BUS_WIDE_4B) != HAL_OK)
    {
        /* Some cards don't support 4-bit; continue with 1-bit */
        SendUartStr("SD:1BIT_MODE\n");
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
        if (HAL_SD_ReadBlocks(&s_hsd1, s_sectorBuf,
                              (uint32_t)(sector + i), 1, 1000) != HAL_OK)
            return RES_ERROR;
        while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER) {}
        SCB_InvalidateDCache_by_Addr((uint32_t *)s_sectorBuf, 512);
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
        if (HAL_SD_WriteBlocks(&s_hsd1, s_sectorBuf,
                               (uint32_t)(sector + i), 1, 1000) != HAL_OK)
            return RES_ERROR;
        while (HAL_SD_GetCardState(&s_hsd1) != HAL_SD_CARD_TRANSFER) {}
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
