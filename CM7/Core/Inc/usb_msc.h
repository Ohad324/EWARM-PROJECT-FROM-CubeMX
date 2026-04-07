/**
 * usb_msc.h — Standalone USB Mass Storage task
 *
 * Completely independent of audio_sd.c.
 * Has its own SD_HandleTypeDef and SDMMC1 init/deinit.
 *
 * Usage:
 *   Call USB_MSC_TaskEntry() as a FreeRTOS task from main.c.
 *   The task blocks on g_SysMode == SYS_MODE_USB_MSC,
 *   then starts USB, exposes the SD card to the PC,
 *   and waits until USB is unplugged or SysMode changes.
 */

#ifndef USB_MSC_H
#define USB_MSC_H

#include "FreeRTOS.h"
#include "task.h"

/**
 * USB_MSC_TaskEntry — FreeRTOS task for USB Mass Storage.
 * Stack: 512 words minimum.  Priority: tskIDLE_PRIORITY + 1.
 */
void USB_MSC_TaskEntry(void *argument);

/**
 * USB_MSC_Activate — call from button / BLE handler to switch to USB MSC mode.
 * Safe from any task context.
 */
void USB_MSC_Activate(void);

/**
 * USB_MSC_SDMMC_IRQHandler — call from SDMMC1_IRQHandler in stm32h7xx_it.c
 * when g_SysMode == SYS_MODE_USB_MSC.
 */
void USB_MSC_SDMMC_IRQHandler(void);

/* Storage callbacks — registered in usbd_storage.c via USBD_DISK_fops */
#include <stdint.h>
int8_t USB_MSC_StorageInit(uint8_t lun);
int8_t USB_MSC_StorageGetCapacity(uint8_t lun, uint32_t *block_num, uint16_t *block_size);
int8_t USB_MSC_StorageIsReady(uint8_t lun);
int8_t USB_MSC_StorageIsWriteProtected(uint8_t lun);
int8_t USB_MSC_StorageRead(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
int8_t USB_MSC_StorageWrite(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len);
int8_t USB_MSC_StorageGetMaxLun(void);

#endif /* USB_MSC_H */
