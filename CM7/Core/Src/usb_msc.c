/**
 * usb_msc.c — USB Mass Storage Class for SD card format recovery
 *
 * DESIGN RULES:
 *   - Does NOT include audio_sd.h or call any audio_sd function.
 *   - Has its own private SD_HandleTypeDef (s_hsd_msc) for SDMMC1.
 *   - No dynamic allocation (no malloc/pvPortMalloc/new).
 *
 * HARDWARE:
 *   USB OTG HS with ULPI PHY (USB3320) on CN1 connector.
 *   SDMMC1 on PC8-PC12/PD2 — same pins as audio_sd, never concurrent.
 *
 * BOOT SEQUENCE:
 *   1. Task starts at boot (before recording pipeline is active).
 *   2. Poll blue button (PC13) for 4 seconds.
 *      - Button held entire 4s → enter USB MSC format mode (step 3).
 *      - Button released early or not pressed → vTaskDelete(NULL), done.
 *   3. Release SDMMC1 from audio_sd, start USB Device MSC.
 *   4. PC connects, formats SD as exFAT.
 *   5. Wait for USB cable disconnect (host suspends).
 *   6. NVIC_SystemReset() — clean boot, f_mount finds valid exFAT.
 */

#include "usb_msc.h"
#include "audio_sd.h"         /* AudioSD_DeInit — release SDMMC1 before MSC inits */
#include "log_mutex.h"
#include "itm_log.h"          /* STAGE() — ITM + RTT zero-overhead tracing */

/* Set to 1 when USB MSC owns SDMMC1 — read by SDMMC1_IRQHandler dispatcher */
volatile uint32_t g_usbMscActive = 0u;

/* Force-format flag — fixed at last word of D3 SRAM (0x3800FFFC).
 * __no_init: startup code does NOT zero this — value written by JLink survives reset.
 * JLink writes FORCE_FORMAT_MAGIC here after flash, before CPU runs.
 * USB_MSC_TaskEntry reads it once and clears it. */
#define FORCE_FORMAT_MAGIC  0xF04CA700u
__no_init volatile uint32_t g_forceFormatMode @ 0x3800FFfCu;

/* Format-done flag — written by format_sd.py via JLink after diskpart succeeds.
 * STM32 polls this every 500 ms during the format window.
 * Lives at 0x3800FFF8 (4 bytes before g_forceFormatMode). */
#define FORMAT_DONE_MAGIC  0xD04ED04Eu
__no_init volatile uint32_t g_formatDone @ 0x3800FFF8u;

#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_msc.h"
#include "usbd_storage.h"

#include "stm32h7xx_hal.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdbool.h>
#include <string.h>

/* =========================================================================
 * All USB MSC statics live in D2 SRAM1 (0x30000000) — isolated from the
 * audio pipeline which uses AXI SRAM (D1, 0x24000000).
 * IAR section placement: @ ".usb_msc_bss"
 * Linker script: stm32h747xx_flash_CM7.icf places .usb_msc_bss → SRAM1_region
 * ====================================================================== */

/* Private SD handle — completely independent of audio_sd.c */
#pragma location = ".usb_msc_bss"
static SD_HandleTypeDef s_hsd_msc;

/* USB Device handle */
#pragma location = ".usb_msc_bss"
static USBD_HandleTypeDef s_usbdHandle;

/* Aligned sector buffer for MSC read/write — 32-byte for D-cache coherency.
 * D2 SRAM1 is cacheable; DMA-safe as long as we clean/invalidate around transfers. */
/* 8KB — matches MSC_MEDIA_PACKET in usbd_conf.h for HS throughput */
#pragma data_alignment = 32
#pragma location = ".usb_msc_bss"
static uint8_t s_mscSectorBuf[8 * 1024u];

/* =========================================================================
 * Private helpers
 * ====================================================================== */

static bool s_sdInit(void)
{
    memset(&s_hsd_msc, 0, sizeof(s_hsd_msc));

    s_hsd_msc.Instance                 = SDMMC1;
    s_hsd_msc.Init.ClockEdge           = SDMMC_CLOCK_EDGE_RISING;
    s_hsd_msc.Init.ClockPowerSave      = SDMMC_CLOCK_POWER_SAVE_DISABLE;
    s_hsd_msc.Init.BusWide             = SDMMC_BUS_WIDE_1B;
    s_hsd_msc.Init.HardwareFlowControl = SDMMC_HARDWARE_FLOW_CONTROL_ENABLE;
    s_hsd_msc.Init.ClockDiv            = 4;

    if (HAL_SD_Init(&s_hsd_msc) != HAL_OK) {
        RLOG("[MSC] SD init FAIL");
        return false;
    }
    if (HAL_SD_ConfigWideBusOperation(&s_hsd_msc, SDMMC_BUS_WIDE_4B) != HAL_OK)
        RLOG("[MSC] SD 4-bit config FAIL — continuing 1-bit");
    return true;
}

static bool s_sdWaitReady(uint32_t timeout_ms)
{
    /* Called from USB IRQ context — must NOT use vTaskDelay */
    uint32_t t0 = HAL_GetTick();
    while (HAL_SD_GetCardState(&s_hsd_msc) != HAL_SD_CARD_TRANSFER) {
        if ((HAL_GetTick() - t0) > timeout_ms)
            return false;
    }
    return true;
}

/* =========================================================================
 * SDMMC1 IRQ trampoline — must coexist with audio_sd's trampoline.
 * audio_sd registers its own handler when active; we register ours here.
 * stm32h7xx_it.c calls a shared dispatcher — see note in usb_msc.h.
 *
 * IMPLEMENTATION NOTE:
 *   stm32h7xx_it.c already has SDMMC1_IRQHandler calling
 *   AudioSD_SDMMC_IRQHandler().  When USB MSC is active, that path is
 *   dormant (audio_sd is not running).  We add USB_MSC_SDMMC_IRQHandler()
 *   and dispatch from IT based on g_SysMode.
 * ====================================================================== */
void USB_MSC_SDMMC_IRQHandler(void)
{
    HAL_SD_IRQHandler(&s_hsd_msc);
}

/* =========================================================================
 * MSC storage callbacks — called by usbd_msc.c
 * These are registered via USBD_DISK_fops in usbd_storage.c.
 * usbd_storage.c is intentionally thin — all SD logic lives here.
 * ====================================================================== */

int8_t USB_MSC_StorageInit(uint8_t lun)
{
    (void)lun;
    bool ok = s_sdInit();
    RLOG("[MSC] StorageInit lun=%u result=%d", lun, ok ? 0 : -1);
    return ok ? 0 : -1;
}

int8_t USB_MSC_StorageGetCapacity(uint8_t lun,
                                   uint32_t *block_num,
                                   uint16_t *block_size)
{
    (void)lun;
    HAL_SD_CardInfoTypeDef info;
    if (HAL_SD_GetCardInfo(&s_hsd_msc, &info) != HAL_OK) {
        RLOG("[MSC] GetCapacity FAIL");
        return -1;
    }
    *block_num  = info.LogBlockNbr - 1u;
    *block_size = (uint16_t)info.LogBlockSize;
    RLOG("[MSC] GetCapacity blocks=%lu blksize=%u", *block_num, *block_size);
    return 0;
}

int8_t USB_MSC_StorageIsReady(uint8_t lun)
{
    (void)lun;
    /* Must return immediately — called from USB IRQ context */
    return (HAL_SD_GetCardState(&s_hsd_msc) == HAL_SD_CARD_TRANSFER) ? 0 : -1;
}

int8_t USB_MSC_StorageIsWriteProtected(uint8_t lun)
{
    (void)lun;
    return 0;
}

/* Max blocks per transfer — bounded by s_mscSectorBuf size (8KB = 16 sectors) */
#define MSC_MAX_BLOCKS  16u

int8_t USB_MSC_StorageRead(uint8_t lun,
                            uint8_t *buf,
                            uint32_t blk_addr,
                            uint16_t blk_len)
{
    (void)lun;
    uint16_t remaining = blk_len;
    uint32_t addr      = blk_addr;
    uint8_t *dst       = buf;

    while (remaining > 0u) {
        uint16_t n = (remaining > MSC_MAX_BLOCKS) ? MSC_MAX_BLOCKS : remaining;
        uint32_t bytes = (uint32_t)n * 512u;

        /* D2 SRAM1 is non-cacheable (MPU region 7) — no cache ops needed */
        if (!s_sdWaitReady(2000u)) return -1;
        if (HAL_SD_ReadBlocks(&s_hsd_msc, s_mscSectorBuf, addr, n, 5000u) != HAL_OK) {
            RLOG("[MSC] Read FAIL blk=%lu n=%u err=0x%08lX", addr, n, s_hsd_msc.ErrorCode);
            return -1;
        }
        if (!s_sdWaitReady(5000u)) {
            RLOG("[MSC] Read WaitReady TIMEOUT blk=%lu", addr);
            return -1;
        }
        memcpy(dst, s_mscSectorBuf, bytes);

        addr      += n;
        dst       += bytes;
        remaining -= n;
    }
    return 0;
}

int8_t USB_MSC_StorageWrite(uint8_t lun,
                             uint8_t *buf,
                             uint32_t blk_addr,
                             uint16_t blk_len)
{
    (void)lun;
    uint16_t remaining = blk_len;
    uint32_t addr      = blk_addr;
    uint8_t *src       = buf;

    while (remaining > 0u) {
        uint16_t n = (remaining > MSC_MAX_BLOCKS) ? MSC_MAX_BLOCKS : remaining;
        uint32_t bytes = (uint32_t)n * 512u;
        /* D2 SRAM1 is non-cacheable (MPU region 7) — no cache ops needed */
        memcpy(s_mscSectorBuf, src, bytes);
        if (!s_sdWaitReady(2000u)) return -1;
        if (HAL_SD_WriteBlocks(&s_hsd_msc, s_mscSectorBuf, addr, n, 5000u) != HAL_OK) {
            RLOG("[MSC] Write FAIL blk=%lu n=%u err=0x%08lX", addr, n, s_hsd_msc.ErrorCode);
            return -1;
        }
        if (!s_sdWaitReady(5000u)) {
            RLOG("[MSC] Write WaitReady TIMEOUT blk=%lu", addr);
            return -1;
        }

        addr      += n;
        src       += bytes;
        remaining -= n;
    }
    return 0;
}

int8_t USB_MSC_StorageGetMaxLun(void)
{
    return 0;
}

/* =========================================================================
 * Task entry — boot-time format recovery via 4-second button hold.
 *
 * Blue button PC13: active HIGH (press = GPIO_PIN_SET).
 * Hold for 4 continuous seconds at boot → USB MSC activates.
 * Release early or no press → task self-deletes, USB never starts.
 * After PC formats SD: cable unplug detected → NVIC_SystemReset().
 * ====================================================================== */
void USB_MSC_TaskEntry(void *argument)
{
    (void)argument;

    /* ── Step 1: enter format mode via button OR JLink flag ────────────── */
    STAGE("MSC1 PASS");

#define MSC_HOLD_MS      4000u
#define MSC_POLL_MS        50u

    bool forceMode = (g_forceFormatMode == FORCE_FORMAT_MAGIC);
    g_forceFormatMode = 0u;   /* clear immediately — one-shot */

    if (!forceMode)
    {
        /* Normal path: require 4-second continuous button hold */
        uint32_t held    = 0u;
        uint32_t elapsed = 0u;

        while (elapsed < MSC_HOLD_MS)
        {
            if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_SET)
                held += MSC_POLL_MS;
            else
                held = 0u;

            elapsed += MSC_POLL_MS;
            vTaskDelay(pdMS_TO_TICKS(MSC_POLL_MS));
        }

        if (held < MSC_HOLD_MS)
        {
            STAGE("Mass storage - terminated");
            vTaskDelete(NULL);
            return;
        }
    }

    /* ── Step 2: Format mode activated ─────────────────────────────────── */
    STAGE("MSC2 PASS");

    /* Configure MPU: mark D2 SRAM1 (0x30000000, 512KB) as non-cacheable.
     * Covers s_mscSectorBuf, s_hsd_msc, s_usbdHandle — all in .usb_msc_bss. */
    HAL_MPU_Disable();
    MPU_Region_InitTypeDef mpu = {0};
    mpu.Enable           = MPU_REGION_ENABLE;
    mpu.Number           = MPU_REGION_NUMBER7;
    mpu.BaseAddress      = 0x30000000u;
    mpu.Size             = MPU_REGION_SIZE_512KB;
    mpu.SubRegionDisable = 0x00u;
    mpu.TypeExtField     = MPU_TEX_LEVEL1;
    mpu.AccessPermission = MPU_REGION_FULL_ACCESS;
    mpu.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
    mpu.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
    mpu.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
    mpu.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
    HAL_MPU_ConfigRegion(&mpu);
    HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

    /* Signal IRQ dispatcher: SDMMC1 now belongs to USB MSC */
    g_usbMscActive = 1u;
    __DSB();

    /* Release audio_sd SDMMC1 handle — USB MSC will init its own */
    AudioSD_DeInit();

    /* Required for ULPI PHY — must be called before USBD_Init */
    HAL_PWREx_EnableUSBVoltageDetector();

    /* Init and start USB Device */
    memset(&s_usbdHandle, 0, sizeof(s_usbdHandle));
    USBD_Init(&s_usbdHandle, &MSC_Desc, 0);
    USBD_RegisterClass(&s_usbdHandle, USBD_MSC_CLASS);
    USBD_MSC_RegisterStorage(&s_usbdHandle, &USBD_DISK_fops);
    USBD_Start(&s_usbdHandle);
    STAGE("MSC3 PASS");   /* D+ pulled up — host will enumerate */

    /* ── Cable presence check — host sends USB reset within 100ms ──────── */
    vTaskDelay(pdMS_TO_TICKS(300u));
    if (s_usbdHandle.dev_state > 0u)
        { STAGE("MSC CABLE PASS"); }   /* USB reset received — cable connected */
    else
        { STAGE("MSC CABLE FAIL"); }   /* no USB activity — cable not connected */

    /* ── Wait for PC enumeration (up to 30s) ───────────────────────────── */
    uint32_t wait = 300u;              /* already waited 300ms above */
    while (s_usbdHandle.dev_state != USBD_STATE_CONFIGURED && wait < 30000u)
        { vTaskDelay(pdMS_TO_TICKS(200u)); wait += 200u; }

    if (s_usbdHandle.dev_state == USBD_STATE_CONFIGURED)
        { STAGE("MSC4 PASS"); }   /* PC enumerated — drive visible, format running */
    else
        { STAGE("MSC4 FAIL"); }   /* no enumeration after 30s */

    /* ── Wait for USB suspend: format_sd.py ejects the disk after diskpart ─ */
    /* When Python runs "offline disk", Windows suspends USB → dev_state changes */
    uint32_t waited = 0u;
    while (s_usbdHandle.dev_state == USBD_STATE_CONFIGURED && waited < 60000u) {
        vTaskDelay(pdMS_TO_TICKS(500u));
        waited += 500u;
    }
    STAGE("FMT DONE");   /* USB suspended = drive ejected = format complete */
    NVIC_SystemReset();
}
