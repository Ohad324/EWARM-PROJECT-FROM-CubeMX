/**
 * usb_msc.c — Standalone USB Mass Storage Class task
 *
 * DESIGN RULES:
 *   - Does NOT include audio_sd.h or call any audio_sd function.
 *   - Has its own private SD_HandleTypeDef (s_hsd_msc) for SDMMC1.
 *   - Uses g_SysMode as the handshake with VoiceRecTask / SDWriteTask:
 *       SYS_MODE_USB_MSC  → this module owns SDMMC1 and USB
 *       SYS_MODE_RECORD   → audio_sd.c owns SDMMC1, USB inactive
 *   - No dynamic allocation (no malloc/pvPortMalloc/new).
 *   - No vTaskDelay as a timing mechanism — uses event flags.
 *
 * HARDWARE:
 *   USB2_OTG_FS on CN1 connector (PA11=DM, PA12=DP), Full-Speed 12 Mbit/s.
 *   SDMMC1 on PC8-PC12/PD2 — same pins as audio_sd, never concurrent.
 *
 * SEQUENCE:
 *   1. Wait for g_SysMode == SYS_MODE_USB_MSC
 *   2. Init SDMMC1 with own handle
 *   3. Init + start USB Device (MSC class)
 *   4. Log "USB MSC ACTIVE"
 *   5. Poll until SYS_MODE_RECORD (user deactivates) or cable disconnect
 *   6. Stop + deinit USB
 *   7. Deinit own SDMMC1 handle  (audio_sd reinits its own on next recording)
 *   8. Set g_SysMode = SYS_MODE_RECORD
 *   9. Loop back to step 1
 */

#include "usb_msc.h"
#include "voice_recorder.h"   /* g_SysMode, SYS_MODE_USB_MSC, SYS_MODE_RECORD */
#include "audio_sd.h"         /* AudioSD_DeInit — release SDMMC1 before MSC inits */
#include "log_mutex.h"

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
    if (HAL_SD_ConfigWideBusOperation(&s_hsd_msc, SDMMC_BUS_WIDE_4B) != HAL_OK) {
        RLOG("[MSC] SD 4-bit config FAIL");
        /* Continue anyway in 1-bit mode */
    }
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
 * Task suspension helpers
 *
 * All application tasks are suspended by name when USB MSC activates.
 * They are NEVER resumed — USB MSC mode is a one-way, permanent takeover
 * until the board is reset.  The USB task itself and the FreeRTOS timer/
 * idle tasks are left running so HAL_Delay and vTaskDelay keep working.
 * ====================================================================== */

/* Names of every app task that must be frozen while USB MSC owns the SD card */
static const char * const k_tasksToFreeze[] = {
    "VoiceRecTask",
    "SDWriteTask",
    "RTTLogTask",
    "HealthMonTask",
    "VoiceCMDhandler",
    "rtos_trace",
    "TouchGFX",
    "VideoTask",
    "UARTReceive",
    /* NOTE: IDLE task must NOT be suspended — scheduler requires it */
};
#define FREEZE_COUNT  (sizeof(k_tasksToFreeze) / sizeof(k_tasksToFreeze[0]))

static void s_freezeAllOtherTasks(void)
{
    for (uint32_t i = 0u; i < FREEZE_COUNT; i++) {
        TaskHandle_t h = xTaskGetHandle(k_tasksToFreeze[i]);
        if (h != NULL)
            vTaskSuspend(h);
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

void USB_MSC_Activate(void)
{
    g_SysMode = SYS_MODE_USB_MSC;
    __DSB();
}

/* =========================================================================
 * Task entry — one-way: once activated, other tasks are frozen forever.
 * Only a board reset returns to normal operation.
 * ====================================================================== */
void USB_MSC_TaskEntry(void *argument)
{
    (void)argument;

    /* Wait until USB_MSC_Activate() is called */
    while (g_SysMode != SYS_MODE_USB_MSC)
        vTaskDelay(pdMS_TO_TICKS(100u));

    RLOG("[MSC] USB MSC ACTIVATED — freezing all other tasks");

    /* Let any in-flight SD/audio operation complete */
    vTaskDelay(pdMS_TO_TICKS(300u));

    /* Freeze every other app task — permanent until reset */
    s_freezeAllOtherTasks();

    /* Configure MPU: mark D2 SRAM1 (0x30000000, 512KB) as non-cacheable.
     * This covers .usb_msc_bss (s_mscSectorBuf, s_usbMem, s_hsd_msc, s_usbdHandle).
     * Eliminates all cache coherency issues for USB DMA and SD DMA buffers. */
    HAL_MPU_Disable();
    MPU_Region_InitTypeDef mpu = {0};
    mpu.Enable           = MPU_REGION_ENABLE;
    mpu.Number           = MPU_REGION_NUMBER7;   /* use region 7 — highest priority */
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
    RLOG("[MSC] MPU: D2 SRAM1 marked non-cacheable");

    RLOG("[MSC] All tasks frozen. Unmounting FatFS and releasing SDMMC1...");

    /* Unmount FatFS and release audio_sd's SDMMC1 handle.
     * SD init is deferred — s_sdInit() will be called lazily by USB_MSC_StorageInit
     * when the PC host connects and enumerates the device.  Do NOT touch the SD
     * hardware here — the host is the one that "wakes" the storage. */
    AudioSD_DeInit();

    /* Required for ULPI PHY — must be called before USBD_Init */
    HAL_PWREx_EnableUSBVoltageDetector();

    /* Init and start USB Device */
    memset(&s_usbdHandle, 0, sizeof(s_usbdHandle));
    USBD_StatusTypeDef r1 = USBD_Init(&s_usbdHandle, &MSC_Desc, 0);
    USBD_StatusTypeDef r2 = USBD_RegisterClass(&s_usbdHandle, USBD_MSC_CLASS);
    USBD_StatusTypeDef r3 = USBD_MSC_RegisterStorage(&s_usbdHandle, &USBD_DISK_fops);
    USBD_StatusTypeDef r4 = USBD_Start(&s_usbdHandle);
    RLOG("[MSC] USBD_Init=%d RegClass=%d RegStorage=%d Start=%d", r1, r2, r3, r4);

    RLOG("[MSC] USB MSC ACTIVE — connect CN1 to PC. Reset board to exit.");

    /* Run forever — USB interrupts drive all SD transfers from here */
    for (;;)
        vTaskDelay(pdMS_TO_TICKS(1000u));
}
