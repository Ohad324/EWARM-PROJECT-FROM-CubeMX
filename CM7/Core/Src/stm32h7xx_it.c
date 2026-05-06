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

#ifndef RELEASE_BUILD
/* IRQ ring-buffer trace. ETM-substitute. Each ISR's first instruction
 * appends (irq_num, DWT cycle count) into the ring. Buffer at fixed
 * address 0x24050000 (320 KB into AXI SRAM, ABOVE the partial framebuffer
 * at 0x24000000-0x24046800) so J-Link can read non-invasively via mem8/
 * mem32 commands at any time. Was 0x24008000 before PFB landed; relocated
 * to avoid collision with the new strip framebuffer. */
typedef struct __attribute__((packed)) {
    uint8_t  irq_num;
    uint8_t  pad[3];
    uint32_t cycle;
} irq_log_entry_t;

#define IRQ_LOG_SIZE 256u

#pragma location = 0x24050000
__root volatile irq_log_entry_t g_irq_log[IRQ_LOG_SIZE];
#pragma location = 0x24050800
__root volatile uint32_t        g_irq_idx = 0;   /* zero-init so reads are clean */

static inline void Log_IRQ(uint8_t irq_num)
{
    /* No-wrap mode: capture only the first IRQ_LOG_SIZE events from boot
     * so we can see startup behavior. Once full, ignore further IRQs. */
    uint32_t i = g_irq_idx;
    if (i < IRQ_LOG_SIZE)
    {
        g_irq_log[i].irq_num = irq_num;
        g_irq_log[i].cycle   = DWT->CYCCNT;
        g_irq_idx = i + 1u;
    }
}
#else
#define Log_IRQ(x) ((void)0)
#endif

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
/* extern void USB_MSC_SDMMC_IRQHandler(void); */   /* USB MSC disabled */
/* g_usbMscActive: set to 1 by usb_msc.c when USB MSC owns SDMMC1 */
/* extern volatile uint32_t g_usbMscActive; */      /* USB MSC disabled */
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
  SEGGER_RTT_WriteString(0, "[FATAL] HardFault! Check stack HWM — VoiceRecTask overflow suspected\n");
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
  /* TIM6 fires at 1 kHz — would flood the 256-entry IRQ logger and
   * overwrite LTDC_ER/DMA2D/DSI events of interest. Don't log. */
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
  Log_IRQ(88);
  /* USER CODE END LTDC_IRQn 0 */
  HAL_LTDC_IRQHandler(&hltdc);
  /* USER CODE BEGIN LTDC_IRQn 1 */

  /* USER CODE END LTDC_IRQn 1 */
}

#ifndef RELEASE_BUILD
/**
 * LTDC global ERROR interrupt — catches FIFO underrun (FUIF) and
 * transfer error (TERRIF). Logs each error to RTT and clears the
 * status flags via ICR so the next refresh can proceed cleanly.
 * Without this handler an LTDC error would just leave IRQ 89 pending
 * forever in NVIC and the CPU would never know.
 */
void LTDC_ER_IRQHandler(void)
{
    Log_IRQ(89);
    extern LTDC_HandleTypeDef hltdc;
    uint32_t isr = LTDC->ISR;
    if (isr & LTDC_ISR_FUIF)
        SEGGER_RTT_WriteString(0, "[LTDC-ERR] FUIF FIFO Underrun -- SDRAM bandwidth starvation\n");
    if (isr & LTDC_ISR_TERRIF)
        SEGGER_RTT_WriteString(0, "[LTDC-ERR] TERRIF Transfer Error -- AXI bus fault during read\n");
    LTDC->ICR = (isr & (LTDC_ISR_FUIF | LTDC_ISR_TERRIF));
    (void)hltdc;
}
#endif

/**
  * @brief This function handles DMA2D global interrupt.
  */
void DMA2D_IRQHandler(void)
{
  /* USER CODE BEGIN DMA2D_IRQn 0 */
  Log_IRQ(90);
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
  Log_IRQ(121);
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
  Log_IRQ(122);
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
  Log_IRQ(123);
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
 * BDMA_Channel1_IRQHandler — SAI4_A RX kick buffer (D3 domain).
 * BDMA runs in circular mode, draining the SAI4 FIFO to keep PE2 clocking.
 * No callbacks needed — HAL_DMA_IRQHandler clears flags and restarts circular. */
void BDMA_Channel1_IRQHandler(void)
{
    extern DMA_HandleTypeDef hdma_sai4_a_rx;
    HAL_DMA_IRQHandler(&hdma_sai4_a_rx);
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
    /* USB MSC disabled — always route to audio_sd */
    /* if (g_usbMscActive)            */
    /*     USB_MSC_SDMMC_IRQHandler(); */
    /* else                           */
        AudioSD_SDMMC_IRQHandler();
}

void OTG_HS_IRQHandler(void)
{
    extern PCD_HandleTypeDef *USB_MSC_GetPCDHandle(void);
    HAL_PCD_IRQHandler(USB_MSC_GetPCDHandle());
}

/* USER CODE END 1 */
