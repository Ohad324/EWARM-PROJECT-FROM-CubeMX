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
 * J-Link command: mem32 0x24000050 18
 *
 * RTTLogTask refreshes all fields every 200 ms. Pin this address in IAR
 * Live Watch to monitor the full audio pipeline without RTT output.
 *
 * Fields [14..16] are decoded extracts of hardware registers for
 * easy reading — no bit-math needed in the debugger.
 */
typedef struct {
    /* ── DFSDM Channel 0 registers ─────────────────────────────────── */
    /* [0]  CH0CFGR1  Expect 0x8018008D (DFSDMEN+CKOUTDIV=24+CHEN+SPICKSEL=11+SITP=01) */
    uint32_t ch0_cfg1;
    /* [1]  CH0CFGR2  Expect 0x00000030 (DTRBS=6 → bits[7:3]=0x06) */
    uint32_t ch0_cfg2;

    /* ── DFSDM Filter 0 registers ──────────────────────────────────── */
    /* [2]  FLTCR1    Expect 0x20240001 (RCSEL=0=CH0, RDMAEN=1, DFEN=1) */
    uint32_t flt0_cr1;
    /* [3]  FLTCR2    Expect 0x00000000 (no interrupt enables) */
    uint32_t flt0_cr2;
    /* [4]  FLTFCR    Expect 0x607C0000 (Sinc3 order=3, FOSR=124 = OSR-1) */
    uint32_t flt0_fcr;
    /* [5]  FLTISR    bits[23:16]=CKABF (sticky OK), bit3=ROVRF (overrun) */
    uint32_t flt0_isr;

    /* ── SAI4 ───────────────────────────────────────────────────────── */
    /* [6]  SAI4 PDMCR  Expect 0x00000101 (PDMEN=1, CKEN1=1) */
    uint32_t sai4_pdm;

    /* ── DMA1 Stream 1 ──────────────────────────────────────────────── */
    /* [7]  DMA CR    bit0=EN. Expect 0x00035500 idle / 0x00035501 active */
    uint32_t dma_cr;
    /* [8]  DMA NDTR  Counts down during recording. Stuck = DMA dead. */
    uint32_t dma_ndtr;
    /* [9]  DMA M0AR  Expect 0x30000000 (buffer base address) */
    uint32_t dma_m0ar;

    /* ── Decoded fields (no bit-math in debugger) ───────────────────── */
    /* [10] SITP      bits[1:0] of ch0_cfg1. Expect 1 = falling edge (LR=HIGH THIS BOARD) */
    uint32_t sitp;
    /* [11] SPICKSEL  bits[3:2] of ch0_cfg1. Expect 3 = 11b = SAI4 bridge */
    uint32_t spicksel;
    /* [12] DTRBS     bits[7:3] of ch0_cfg2. Expect 6. (5 = DC overflow bug!) */
    uint32_t dtrbs;

    /* ── Live audio data ─────────────────────────────────────────────── */
    /* [13] last_raw  Raw FLTRDATAR (uint32_t). Expect oscillating. 0xFF88CA00 = silence. */
    uint32_t last_raw;
    /* [14] last_val  (int32_t)FLTRDATAR>>8. Expect ±30517 active. -30518 = silence/DC. */
    int32_t  last_val;

    /* ── Housekeeping ────────────────────────────────────────────────── */
    /* [15] uptime    Increments every 200 ms. Proves RTTLogTask is alive. */
    uint32_t uptime_ticks;

    /* ── Reference values for Live Watch comparison ──────────────────── */
    /* [16] LR=Low  (LEFT)  ch0_cfg1 with SITP=00 rising  = 0x8018008C */
    uint32_t ref_lr_low;
    /* [17] LR=High (RIGHT) ch0_cfg1 with SITP=01 falling = 0x8018008D ← THIS BOARD */
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
