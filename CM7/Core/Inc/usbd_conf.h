/**
 * usbd_conf.h — USB Device library configuration for STM32H747I-DISCO
 * Matches ST reference: STM32Cube_FW_H7_V1.12.1 MSC_Standalone (USE_USB_HS)
 */
#ifndef __USBD_CONF_H
#define __USBD_CONF_H

#include "stm32h7xx_hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Common Config */
#define USBD_MAX_NUM_INTERFACES               1
#define USBD_MAX_NUM_CONFIGURATION            1
#define USBD_MAX_STR_DESC_SIZ                 64
#define USBD_SELF_POWERED                     1
#define USBD_DEBUG_LEVEL                      0

/* MSC Class Config — 8KB packet for HS throughput */
#define MSC_MEDIA_PACKET                      (8 * 1024)

/* Memory management macros */
#define USBD_malloc         (void *)USBD_static_malloc
#define USBD_free           USBD_static_free
#define USBD_memset         memset
#define USBD_memcpy         memcpy
#define USBD_Delay          HAL_Delay

/* Debug macros (disabled) */
#if (USBD_DEBUG_LEVEL > 0)
#define USBD_UsrLog(...)    printf(__VA_ARGS__); printf("\n");
#else
#define USBD_UsrLog(...)
#endif
#define USBD_ErrLog(...)
#define USBD_DbgLog(...)

void *USBD_static_malloc(uint32_t size);
void  USBD_static_free(void *p);

#endif /* __USBD_CONF_H */
