/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    stm32h7xx_it.c
  * @brief   Interrupt Service Routines.
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

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "stm32h7xx_it.h"
/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "log_mutex.h"   /* LOG() -> SEGGER_RTT_Write, no SEGGER_RTT_printf needed */
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/* External variables --------------------------------------------------------*/
extern DMA2D_HandleTypeDef hdma2d;
extern DSI_HandleTypeDef hdsi;
extern MDMA_HandleTypeDef hmdma_jpeg_infifo_th;
extern MDMA_HandleTypeDef hmdma_jpeg_outfifo_th;
extern JPEG_HandleTypeDef hjpeg;
extern LTDC_HandleTypeDef hltdc;
extern TIM_HandleTypeDef htim6;

/* USER CODE BEGIN EV */
extern UART_HandleTypeDef huart8;
/* BLE_UART_DMA_IRQHandler() wraps HAL_DMA_IRQHandler(&s_hdma_uart8_rx).
   The DMA handle is private to ble_uart.c; this trampoline avoids exposing it. */
extern void BLE_UART_DMA_IRQHandler(void);
/* VoiceRec_DMA_IRQHandler() wraps HAL_DMA_IRQHandler(&s_hdma_dfsdm).
   The DMA handle is private to voice_recorder.c; same pattern as BLE UART. */
extern void VoiceRec_DMA_IRQHandler(void);
/* AudioSD_SDMMC_IRQHandler() wraps HAL_SD_IRQHandler(&s_hsd1).
   The SD handle is private to audio_sd.c; same trampoline pattern. */
extern void AudioSD_SDMMC_IRQHandler(void);
/* USB_MSC_SDMMC_IRQHandler() wraps HAL_SD_IRQHandler(&s_hsd_msc).
   Active only while USB MSC format mode owns SDMMC1. */
extern void USB_MSC_SDMMC_IRQHandler(void);
/* g_usbMscActive: set to 1 by usb_msc.c when USB MSC owns SDMMC1 */
extern volatile uint32_t g_usbMscActive;
/* USER CODE END EV */

/******************************************************************************/
/*           Cortex Processor Interruption and Exception Handlers          */
/******************************************************************************/
/**
  * @brief This function handles Non maskable interrupt.
  */
void NMI_Handler(void)
{
  /* USER CODE BEGIN NonMaskableInt_IRQn 0 */

  /* USER CODE END NonMaskableInt_IRQn 0 */
  /* USER CODE BEGIN NonMaskableInt_IRQn 1 */
  while (1)
  {
  }
  /* USER CODE END NonMaskableInt_IRQn 1 */
}

/**
  * @brief This function handles Hard fault interrupt.
  */
void HardFault_Handler(void)
{
  /* USER CODE BEGIN HardFault_IRQn 0 */

  /* USER CODE END HardFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_HardFault_IRQn 0 */
    /* USER CODE END W1_HardFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Memory management fault.
  */
void MemManage_Handler(void)
{
  /* USER CODE BEGIN MemoryManagement_IRQn 0 */
  uint32_t cfsr  = SCB->CFSR;
  uint32_t mmfar = SCB->MMFAR;
  LOG("[FATAL] MemManage! CFSR=%08lX MMFAR=%08lX\n", cfsr, mmfar);
  /* USER CODE END MemoryManagement_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_MemoryManagement_IRQn 0 */
    /* USER CODE END W1_MemoryManagement_IRQn 0 */
  }
}

/**
  * @brief This function handles Pre-fetch fault, memory access fault.
  */
void BusFault_Handler(void)
{
  /* USER CODE BEGIN BusFault_IRQn 0 */
  uint32_t cfsr = SCB->CFSR;
  uint32_t bfar = SCB->BFAR;
  LOG("[FATAL] BusFault! CFSR=%08lX BFAR=%08lX\n", cfsr, bfar);
  /* USER CODE END BusFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_BusFault_IRQn 0 */
    /* USER CODE END W1_BusFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Undefined instruction or illegal state.
  */
void UsageFault_Handler(void)
{
  /* USER CODE BEGIN UsageFault_IRQn 0 */
  LOG("[FATAL] UsageFault! CFSR=%08lX\n", SCB->CFSR);
  /* USER CODE END UsageFault_IRQn 0 */
  while (1)
  {
    /* USER CODE BEGIN W1_UsageFault_IRQn 0 */
    /* USER CODE END W1_UsageFault_IRQn 0 */
  }
}

/**
  * @brief This function handles Debug monitor.
  */
void DebugMon_Handler(void)
{
  /* USER CODE BEGIN DebugMonitor_IRQn 0 */

  /* USER CODE END DebugMonitor_IRQn 0 */
  /* USER CODE BEGIN DebugMonitor_IRQn 1 */

  /* USER CODE END DebugMonitor_IRQn 1 */
}

/******************************************************************************/
/* STM32H7xx Peripheral Interrupt Handlers                                    */
/* Add here the Interrupt Handlers for the used peripherals.                  */
/* For the available peripheral interrupt handler names,                      */
/* please refer to the startup file (startup_stm32h7xx.s).                    */
/******************************************************************************/

/**
  * @brief This function handles TIM6 global interrupt, DAC1_CH1 and DAC1_CH2 underrun error interrupts.
  */
void TIM6_DAC_IRQHandler(void)
{
  /* USER CODE BEGIN TIM6_DAC_IRQn 0 */

  /* USER CODE END TIM6_DAC_IRQn 0 */
  HAL_TIM_IRQHandler(&htim6);
  /* USER CODE BEGIN TIM6_DAC_IRQn 1 */

  /* USER CODE END TIM6_DAC_IRQn 1 */
}

/**
  * @brief This function handles LTDC global interrupt.
  */
void LTDC_IRQHandler(void)
{
  /* USER CODE BEGIN LTDC_IRQn 0 */

  /* USER CODE END LTDC_IRQn 0 */
  HAL_LTDC_IRQHandler(&hltdc);
  /* USER CODE BEGIN LTDC_IRQn 1 */

  /* USER CODE END LTDC_IRQn 1 */
}

/**
  * @brief This function handles DMA2D global interrupt.
  */
void DMA2D_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2D_IRQn 0 */

  /* USER CODE END DMA2D_IRQn 0 */
  HAL_DMA2D_IRQHandler(&hdma2d);
  /* USER CODE BEGIN DMA2D_IRQn 1 */

  /* USER CODE END DMA2D_IRQn 1 */
}

/**
  * @brief This function handles JPEG global interrupt.
  */
void JPEG_IRQHandler(void)
{
  /* USER CODE BEGIN JPEG_IRQn 0 */

  /* USER CODE END JPEG_IRQn 0 */
  HAL_JPEG_IRQHandler(&hjpeg);
  /* USER CODE BEGIN JPEG_IRQn 1 */

  /* USER CODE END JPEG_IRQn 1 */
}

/**
  * @brief This function handles MDMA global interrupt.
  */
void MDMA_IRQHandler(void)
{
  /* USER CODE BEGIN MDMA_IRQn 0 */

  /* USER CODE END MDMA_IRQn 0 */
  HAL_MDMA_IRQHandler(&hmdma_jpeg_outfifo_th);
  HAL_MDMA_IRQHandler(&hmdma_jpeg_infifo_th);
  /* USER CODE BEGIN MDMA_IRQn 1 */

  /* USER CODE END MDMA_IRQn 1 */
}

/**
  * @brief This function handles DSI global Interrupt.
  */
void DSI_IRQHandler(void)
{
  /* USER CODE BEGIN DSI_IRQn 0 */

  /* USER CODE END DSI_IRQn 0 */
  HAL_DSI_IRQHandler(&hdsi);
  /* USER CODE BEGIN DSI_IRQn 1 */

  /* USER CODE END DSI_IRQn 1 */
}

/* USER CODE BEGIN 1 */

/**
 * UART8_IRQHandler — handles UART8 events including IDLE line detection.
 *
 * HAL_UART_IRQHandler detects the IDLE flag and calls
 * HAL_UARTEx_RxEventCallback (in ble_uart.c), which posts to xRawBleQueue
 * and restarts DMA.  Priority 5 (set in HAL_UART_MspInit): the highest
 * level allowed to call FreeRTOS ISR-safe APIs.
 */
void UART8_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart8);
}

/**
 * DMA1_Stream0_IRQHandler — handles DMA transfer-complete for UART8 RX.
 *
 * Fires when s_dma_rx_buf[] is completely filled (buffer-full case for
 * messages > 128 bytes).  Routed through BLE_UART_DMA_IRQHandler() to keep
 * the DMA handle private to ble_uart.c.  Priority 6: one notch below UART8
 * IRQ (5) so IDLE detection can pre-empt a simultaneous DMA-TC event.
 */
void DMA1_Stream0_IRQHandler(void)
{
    BLE_UART_DMA_IRQHandler();
}

/**
 * EXTI15_10_IRQHandler — handles external interrupts on GPIO lines 10-15.
 * PC13 (blue wakeup button) is on EXTI line 13, which falls in this range.
 * HAL_GPIO_EXTI_IRQHandler clears the pending flag and calls
 * HAL_GPIO_EXTI_Callback (defined in voice_recorder.c) with GPIO_PIN_13.
 */
void EXTI15_10_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(GPIO_PIN_13); /* clear flag + dispatch to callback */
}

/**
 * DMA1_Stream1_IRQHandler — handles DFSDM1 Filter0 DMA transfer events.
 * Fires on half-complete and complete (PCM data from DFSDM hardware filter ready).
 * Routed through VoiceRec_DMA_IRQHandler() to keep the DMA handle private
 * to voice_recorder.c — same pattern used by BLE UART on DMA1_Stream0.
 * Note: DFSDM1 uses DMA1 (D2 domain), buffer in D2 SRAM (0x30000000).
 */
void DMA1_Stream1_IRQHandler(void)
{
    VoiceRec_DMA_IRQHandler(); /* delegate to voice_recorder.c private handler */
}

/**
 * SDMMC1_IRQHandler — handles SDMMC1 transfer and command completion events.
 * Routed through AudioSD_SDMMC_IRQHandler() to keep the SD handle private
 * to audio_sd.c.  Priority 5: FreeRTOS-safe level.
 */
void SDMMC1_IRQHandler(void)
{
    if (g_usbMscActive)
        USB_MSC_SDMMC_IRQHandler();
    else
        AudioSD_SDMMC_IRQHandler();
}

void OTG_HS_IRQHandler(void)
{
    extern PCD_HandleTypeDef *USB_MSC_GetPCDHandle(void);
    HAL_PCD_IRQHandler(USB_MSC_GetPCDHandle());
}

/* USER CODE END 1 */
