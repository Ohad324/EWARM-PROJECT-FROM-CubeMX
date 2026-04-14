/**
 * usb_msc.h — USB Mass Storage for SD card format recovery
 *
 * Hold blue button (PC13) for 4s at boot → USB HS MSC activates on CN1.
 * PC formats SD as exFAT → unplug cable → board resets → normal recording.
 */

#ifndef USB_MSC_H
#define USB_MSC_H

#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

/**
 * g_usbMscActive — set to 1 when USB MSC owns SDMMC1.
 * Read by SDMMC1_IRQHandler in stm32h7xx_it.c to dispatch correctly.
 * Also checked by VoiceRecTask to skip recording during format mode.
 */
extern volatile uint32_t g_usbMscActive;

/**
 * g_forceFormatMode — written by JLink (0xF04CA700) to bypass button poll.
 * __no_init at 0x3800FFFC — survives software reset, cleared on first read.
 */
extern volatile uint32_t g_forceFormatMode;

/**
 * USB_MSC_TaskEntry — FreeRTOS task entry. Stack: 512 words. Priority: 2.
 * Polls blue button for 4s. Activates USB MSC if held; self-deletes otherwise.
 */
void USB_MSC_TaskEntry(void *argument);

/**
 * USB_MSC_SDMMC_IRQHandler — call from SDMMC1_IRQHandler when g_usbMscActive == 1.
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
