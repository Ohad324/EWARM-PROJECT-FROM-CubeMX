/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * <h2><center>&copy; Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.</center></h2>
  *
  * This software component is licensed by ST under Ultimate Liberty license
  * SLA0044, the "License"; You may not use this file except in compliance with
  * the License. You may obtain a copy of the License at:
  *                             www.st.com/SLA0044
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stm32h747i_discovery_qspi.h"
#include "stm32h747i_discovery_sdram.h"
#include "stm32h747i_discovery_bus.h"
#include "stm32h747i_discovery_errno.h"
#include "../Components/otm8009a/otm8009a.h"
//#include <TouchGFXGeneratedHAL.hpp>
/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/**
 * DFSDM Mission Control Hub
 * Target Address: 0x24000050 (AXI SRAM D1)
 * J-Link command: mem32 0x24000050 10
 *
 * RTTLogTask refreshes every 200 ms. Pin this address in IAR Live Watch
 * to monitor the full audio pipeline health without any RTT output needed.
 */
typedef struct {
    /* [0] CH0CFGR1   Expect 0x8018008D (Master En, Div 24, CHEN, SAI4 Bridge, Falling) */
    uint32_t ch0_cfg1;
    /* [1] CH0CFGR2   Expect 0x00000030 (DTRBS = 6) */
    uint32_t ch0_cfg2;
    /* [2] FLT0CR1    Expect 0x20240001 (RCSEL=0, RDMAEN=1, DFEN=1) */
    uint32_t flt0_cr1;
    /* [3] FLT0FCR    Expect 0x607C0000 (Sinc3, FOSR=125-1) */
    uint32_t flt0_fcr;
    /* [4] FLT0ISR    Watch Bit 17 (CKABF). If 1, Clock is missing! */
    uint32_t flt0_isr;
    /* [5] SAI4_PDMCR Expect 0x00000101 (PDMEN, CKEN1) */
    uint32_t sai4_pdm;
    /* [6] DMA_NDTR   Should be moving during recording. If stuck, DMA is dead. */
    uint32_t dma_ndtr;
    /* [7] Last Data  FLTRDATAR>>8 with arithmetic shift (int32_t cast preserves sign).
     *               Oscillates ±30517 = mic working. Stuck at -30518 = SITP wrong edge. */
    int32_t  last_val;
    /* [8] Heartbeat  Increments every 200 ms. Proves monitor is running. */
    uint32_t uptime_ticks;
    /* [9]  LR=Low  reference: SITP=00 (rising  edge) → expect 0x8018008C */
    uint32_t ref_lr_low;
    /* [10] LR=High reference: SITP=01 (falling edge) → expect 0x8018008D  ← THIS BOARD */
    uint32_t ref_lr_high;
} DFSDM_Debug_Hub_t;

/* Placed at 0x24000050 (AXI SRAM) — __no_init, startup does not zero it */
extern volatile DFSDM_Debug_Hub_t g_dbg;

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */
extern int32_t DSI_IO_Write(uint16_t ChannelNbr, uint16_t Reg, uint8_t *pData, uint16_t Size);
extern int32_t DSI_IO_Read(uint16_t ChannelNbr, uint16_t Reg, uint8_t *pData, uint16_t Size);
/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LCD_RESET_Pin GPIO_PIN_3
#define LCD_RESET_GPIO_Port GPIOG
#define VSYNC_FREQ_Pin GPIO_PIN_3
#define VSYNC_FREQ_GPIO_Port GPIOJ

/* USER CODE BEGIN Private defines */
#define LED1_Pin         GPIO_PIN_12
#define LED1_GPIO_Port   GPIOI
#define LED2_Pin         GPIO_PIN_13
#define LED2_GPIO_Port   GPIOI
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
