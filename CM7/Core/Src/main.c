/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "FreeRTOS.h"
#include "cmsis_os2.h"
#include "libjpeg.h"
#include "app_touchgfx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <string.h>
#include <stdlib.h>              /* atoi() -- THUMB: size parsing */
#include <stdbool.h>             /* bool, true, false in C */
#include "ble_queue.h"
#include "ble_uart.h"            /* DMA+IDLE driver, bleHistory, BLE_UART_Init() */
#include "audio_rec.h"           /* button-triggered MEMS recording, AudioRec_Init() */
#include "log_mutex.h"           /* LOG() macro — outputs to SEGGER RTT */
#include "music_display_task.h"  /* Music_Init(), xMusicQueue, music_msg_t */
#include "cmsis_os2.h"           /* osKernelGetTickCount() */
#include "timing_log.h"          /* TLOG(), T_US() — RTT timing instrumentation */
#include "rtos_trace.h"          /* RtosTrace_Init(), RtosTrace_DrainTask()     */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#ifndef HSEM_ID_0
#define HSEM_ID_0 (0U) /* HW semaphore 0*/
#endif

/** UARTReceiveTask accumulation buffer size.
 *  Larger than BLE_MSG_LEN (64) so that TRACK: messages (~250 bytes max)
 *  fit without truncation.  RESULT: messages are re-truncated to BLE_MSG_LEN
 *  before being posted to xBleQueue (which has 64-byte slots). */
#define UART_ACCUM_SIZE  512u

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

CRC_HandleTypeDef hcrc;

DMA2D_HandleTypeDef hdma2d;

DSI_HandleTypeDef hdsi;

JPEG_HandleTypeDef hjpeg;
MDMA_HandleTypeDef hmdma_jpeg_infifo_th;
MDMA_HandleTypeDef hmdma_jpeg_outfifo_th;

LTDC_HandleTypeDef hltdc;

QSPI_HandleTypeDef hqspi;

SDRAM_HandleTypeDef hsdram1;

/* Definitions for TouchGFXTask */
osThreadId_t TouchGFXTaskHandle;
const osThreadAttr_t TouchGFXTask_attributes = {
  .name = "TouchGFXTask",
  .stack_size = 4096 * 4,    /* reverted: TouchGFXTask was not the culprit (crash = UARTReceiveTask) */
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for videoTask */
osThreadId_t videoTaskHandle;
const osThreadAttr_t videoTask_attributes = {
  .name = "videoTask",
  .stack_size = 1000 * 4,    /* reverted: videoTask stays blocked on semaphore (no VideoWidget active) */
  .priority = (osPriority_t) osPriorityLow,
};
/* USER CODE BEGIN PV */
OTM8009A_Object_t OTM8009AObj;
OTM8009A_IO_t IOCtx;
UART_HandleTypeDef huart8;

/* BLE UART queue — receives messages from UARTReceiveTask, consumed by Model::tick() */
QueueHandle_t xBleQueue;


/* ── DWT cycle-counter helpers (CM7 @ 480 MHz) ─────────────────────────── */
#define DWT_CYCLES_PER_MS  480000UL
#define DWT_MS(c)          ((c) / DWT_CYCLES_PER_MS)
#define DWT_SNAP()         (DWT->CYCCNT)

/** Timestamps shared with JpegDisplayTask for pipeline timing. */
volatile uint32_t g_t_track = 0u;   /* DWT snap when TRACK: received  */
volatile uint32_t g_t_thumb = 0u;   /* DWT snap when THUMB: complete  */

/* JPEG receive buffer — placed in SDRAM so AXI SRAM is not exhausted.
 * MDMA (used by the JPEG peripheral) can reach SDRAM on STM32H7.
 * 200 KB accommodates maxresdefault (1280x720) JPEGs (~80-150 KB typical).
 * 32-byte aligned for SCB_CleanDCache_by_Addr(). */
static uint8_t s_jpegInBuf[200u * 1024u]
    __attribute__((section(".sdram_bss"), aligned(32)));

/* Binary THUMB: receive state — written exclusively by UARTReceiveTask. */
static bool     s_thumbMode     = false;
static uint32_t s_thumbSize     = 0u;
static uint32_t s_thumbReceived = 0u;

/* Debug: incremented inside HAL_UARTEx_RxEventCallback (ISR context). */
volatile uint32_t g_uartIsrCount = 0u;

/* UARTReceiveTask attributes
 *
 * Priority: osPriorityAboveNormal — one tier above TouchGFXTask (osPriorityNormal).
 *
 * Why higher than TouchGFX?
 *   HAL_UARTEx_RxEventCallback posts to xRawBleQueue and calls
 *   portYIELD_FROM_ISR.  For that yield to actually switch to
 *   UARTReceiveTask immediately, the task must outrank whatever was
 *   running (usually TouchGFXTask).  If priorities were equal, the
 *   scheduler might not switch until the next tick, adding up to 1 ms
 *   latency before the queue is drained and DMA is re-armed.
 *
 *   In practice UARTReceiveTask spends almost all its time blocked on
 *   xQueueReceive (portMAX_DELAY), so the higher priority does not
 *   starve TouchGFX during rendering — it only "steals" CPU for the
 *   few microseconds needed to process one message.
 */
static const osThreadAttr_t uartReceiveTask_attributes = {
    .name       = "UARTReceiveTask",
    .stack_size = 1024 * 4,             /* 1024 words = 4 KB — increased from 512: crash showed SP 288B below base (2336B peak vs 2048B stack) */
    .priority   = (osPriority_t) osPriorityAboveNormal,
};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_MDMA_Init(void);
static void MX_FMC_Init(void);
static void MX_QUADSPI_Init(void);
static void MX_DMA2D_Init(void);
static void MX_DSIHOST_DSI_Init(void);
static void MX_LTDC_Init(void);
static void MX_CRC_Init(void);
static void MX_JPEG_Init(void);
void TouchGFX_Task(void *argument);
extern void videoTaskFunc(void *argument);

/* USER CODE BEGIN PFP */
static void MX_UART8_Init(void);
static void UARTReceiveTask(void *argument);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
/* All logging uses SEGGER RTT — see log_mutex.h */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */
/* USER CODE BEGIN Boot_Mode_Sequence_0 */
  int32_t timeout;
/* USER CODE END Boot_Mode_Sequence_0 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

/* USER CODE BEGIN Boot_Mode_Sequence_1 */
  /* Wait until CPU2 boots and enters in stop mode or timeout*/
  timeout = 0xFFFF;
  while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) != RESET) && (timeout-- > 0));
  /* CM4 sync timeout is non-fatal: CM4 runs a stop-mode stub only.
   * If CM4 never signals (e.g. not flashed), CM7 continues normally. */
  (void)timeout;
/* USER CODE END Boot_Mode_Sequence_1 */
  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();
/* USER CODE BEGIN Boot_Mode_Sequence_2 */
/* When system initialization is finished, Cortex-M7 will release Cortex-M4 by means of
HSEM notification */
/*HW semaphore Clock enable*/
__HAL_RCC_HSEM_CLK_ENABLE();
/*Take HSEM */
HAL_HSEM_FastTake(HSEM_ID_0);
/*Release HSEM in order to notify the CPU2(CM4)*/
HAL_HSEM_Release(HSEM_ID_0,0);
/* wait until CPU2 wakes up from stop mode */
timeout = 0xFFFF;
while((__HAL_RCC_GET_FLAG(RCC_FLAG_D2CKRDY) == RESET) && (timeout-- > 0));
if ( timeout < 0 )
{
Error_Handler();
}
/* USER CODE END Boot_Mode_Sequence_2 */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_MDMA_Init();
  MX_FMC_Init();
  MX_QUADSPI_Init();
  MX_DMA2D_Init();
  MX_DSIHOST_DSI_Init();
  MX_LTDC_Init();
  MX_CRC_Init();
  MX_JPEG_Init();
  MX_LIBJPEG_Init();
  MX_TouchGFX_Init();
  /* Call PreOsInit function */
  MX_TouchGFX_PreOSInit();
  /* USER CODE BEGIN 2 */
  MX_UART8_Init();
  /* Attach DMA1 Stream0 to huart8 and create xRawBleQueue / xBleHistMutex.
     Must run after MX_UART8_Init() (UART handle ready) and before
     osKernelStart() (RTOS objects created here).
     The actual HAL_UARTEx_ReceiveToIdle_DMA() call happens inside
     UARTReceiveTask so the FreeRTOS ISR infrastructure is live first. */
  BLE_UART_Init();
  /* Initialise DFSDM mic, DMA, and button EXTI for audio recording.
     Must run after MX_GPIO_Init() (GPIOC clock already on) and before
     osKernelStart() so RTOS objects (semaphore, queue) are created first. */
  /* AudioRec_Init(); */ /* DISABLED: testing thumbnail pipeline without audio */
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* No mutex needed — LOG() uses SEGGER_RTT_Write which has its own spinlock */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* BLE message queue: 8 slots, each BLE_MSG_LEN bytes */
  xBleQueue = xQueueCreate(8, BLE_MSG_LEN * sizeof(char));
  /* Music queues + JpegDisplayTask */
  Music_Init();
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of TouchGFXTask */
  TouchGFXTaskHandle = osThreadNew(TouchGFX_Task, NULL, &TouchGFXTask_attributes);

  /* creation of videoTask */
  videoTaskHandle = osThreadNew(videoTaskFunc, NULL, &videoTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  osThreadNew(UARTReceiveTask, NULL, &uartReceiveTask_attributes);
  /* AudioRecTask: waits for button press, records mic via DFSDM DMA, sends PCM over UART8.
     Stack 4096 bytes: needs ~1 KB for pcm16[] static buffer + FreeRTOS overhead.
     Priority normal: same as TouchGFX — audio send is bursty, not latency-critical. */
  /* xTaskCreate(AudioRec_TaskEntry, "AudioRec", 4096u, NULL, osPriorityNormal, NULL); */ /* DISABLED */
  /* RTOS trace drain task — prio 1 (lowest app priority), 512-word stack */
  RtosTrace_Init();
  xTaskCreate(RtosTrace_DrainTask, "rtos_trace", 512u, NULL, 1u, NULL);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_DIRECT_SMPS_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 160;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);
}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{

  /* USER CODE BEGIN CRC_Init 0 */

  /* USER CODE END CRC_Init 0 */

  /* USER CODE BEGIN CRC_Init 1 */

  /* USER CODE END CRC_Init 1 */
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_ENABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_ENABLE;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_NONE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_DISABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN CRC_Init 2 */

  /* USER CODE END CRC_Init 2 */

}

/**
  * @brief DMA2D Initialization Function
  * @param None
  * @retval None
  */
static void MX_DMA2D_Init(void)
{

  /* USER CODE BEGIN DMA2D_Init 0 */

  /* USER CODE END DMA2D_Init 0 */

  /* USER CODE BEGIN DMA2D_Init 1 */

  /* USER CODE END DMA2D_Init 1 */
  hdma2d.Instance = DMA2D;
  hdma2d.Init.Mode = DMA2D_R2M;
  hdma2d.Init.ColorMode = DMA2D_OUTPUT_RGB888;
  hdma2d.Init.OutputOffset = 0;
  if (HAL_DMA2D_Init(&hdma2d) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DMA2D_Init 2 */

  /* USER CODE END DMA2D_Init 2 */

}

/**
  * @brief DSIHOST Initialization Function
  * @param None
  * @retval None
  */
static void MX_DSIHOST_DSI_Init(void)
{

  /* USER CODE BEGIN DSIHOST_Init 0 */
  HAL_GPIO_WritePin(GPIOG , GPIO_PIN_3 , GPIO_PIN_RESET);
  HAL_Delay(20);/* wait 20 ms */
  HAL_GPIO_WritePin(GPIOG , GPIO_PIN_3, GPIO_PIN_SET);/* Deactivate XRES */
  HAL_Delay(10);/* Wait for 10ms after releasing XRES before sending commands */
  /* USER CODE END DSIHOST_Init 0 */

  DSI_PLLInitTypeDef PLLInit = {0};
  DSI_HOST_TimeoutTypeDef HostTimeouts = {0};
  DSI_PHY_TimerTypeDef PhyTimings = {0};
  DSI_LPCmdTypeDef LPCmd = {0};
  DSI_CmdCfgTypeDef CmdCfg = {0};

  /* USER CODE BEGIN DSIHOST_Init 1 */

  /* USER CODE END DSIHOST_Init 1 */
  hdsi.Instance = DSI;
  hdsi.Init.AutomaticClockLaneControl = DSI_AUTO_CLK_LANE_CTRL_DISABLE;
  hdsi.Init.TXEscapeCkdiv = 4;
  hdsi.Init.NumberOfLanes = DSI_TWO_DATA_LANES;
  PLLInit.PLLNDIV = 119;
  PLLInit.PLLIDF = DSI_PLL_IN_DIV3;
  PLLInit.PLLODF = DSI_PLL_OUT_DIV2;
  if (HAL_DSI_Init(&hdsi, &PLLInit) != HAL_OK)
  {
    Error_Handler();
  }
  HostTimeouts.TimeoutCkdiv = 1;
  HostTimeouts.HighSpeedTransmissionTimeout = 0;
  HostTimeouts.LowPowerReceptionTimeout = 0;
  HostTimeouts.HighSpeedReadTimeout = 0;
  HostTimeouts.LowPowerReadTimeout = 0;
  HostTimeouts.HighSpeedWriteTimeout = 0;
  HostTimeouts.HighSpeedWritePrespMode = DSI_HS_PM_DISABLE;
  HostTimeouts.LowPowerWriteTimeout = 0;
  HostTimeouts.BTATimeout = 0;
  if (HAL_DSI_ConfigHostTimeouts(&hdsi, &HostTimeouts) != HAL_OK)
  {
    Error_Handler();
  }
  PhyTimings.ClockLaneHS2LPTime = 28;
  PhyTimings.ClockLaneLP2HSTime = 33;
  PhyTimings.DataLaneHS2LPTime = 15;
  PhyTimings.DataLaneLP2HSTime = 25;
  PhyTimings.DataLaneMaxReadTime = 0;
  PhyTimings.StopWaitTime = 0;
  if (HAL_DSI_ConfigPhyTimer(&hdsi, &PhyTimings) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DSI_ConfigFlowControl(&hdsi, DSI_FLOW_CONTROL_BTA) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DSI_SetLowPowerRXFilter(&hdsi, 10000) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DSI_ConfigErrorMonitor(&hdsi, HAL_DSI_ERROR_NONE) != HAL_OK)
  {
    Error_Handler();
  }
  LPCmd.LPGenShortWriteNoP = DSI_LP_GSW0P_ENABLE;
  LPCmd.LPGenShortWriteOneP = DSI_LP_GSW1P_ENABLE;
  LPCmd.LPGenShortWriteTwoP = DSI_LP_GSW2P_ENABLE;
  LPCmd.LPGenShortReadNoP = DSI_LP_GSR0P_ENABLE;
  LPCmd.LPGenShortReadOneP = DSI_LP_GSR1P_ENABLE;
  LPCmd.LPGenShortReadTwoP = DSI_LP_GSR2P_ENABLE;
  LPCmd.LPGenLongWrite = DSI_LP_GLW_ENABLE;
  LPCmd.LPDcsShortWriteNoP = DSI_LP_DSW0P_ENABLE;
  LPCmd.LPDcsShortWriteOneP = DSI_LP_DSW1P_ENABLE;
  LPCmd.LPDcsShortReadNoP = DSI_LP_DSR0P_ENABLE;
  LPCmd.LPDcsLongWrite = DSI_LP_DLW_ENABLE;
  LPCmd.LPMaxReadPacket = DSI_LP_MRDP_ENABLE;
  LPCmd.AcknowledgeRequest = DSI_ACKNOWLEDGE_ENABLE;
  if (HAL_DSI_ConfigCommand(&hdsi, &LPCmd) != HAL_OK)
  {
    Error_Handler();
  }
  CmdCfg.VirtualChannelID = 0;
  CmdCfg.ColorCoding = DSI_RGB888;
  CmdCfg.CommandSize = 400;
  CmdCfg.TearingEffectSource = DSI_TE_EXTERNAL;
  CmdCfg.TearingEffectPolarity = DSI_TE_RISING_EDGE;
  CmdCfg.HSPolarity = DSI_HSYNC_ACTIVE_HIGH;
  CmdCfg.VSPolarity = DSI_VSYNC_ACTIVE_HIGH;
  CmdCfg.DEPolarity = DSI_DATA_ENABLE_ACTIVE_HIGH;
  CmdCfg.VSyncPol = DSI_VSYNC_RISING;
  CmdCfg.AutomaticRefresh = DSI_AR_DISABLE;
  CmdCfg.TEAcknowledgeRequest = DSI_TE_ACKNOWLEDGE_ENABLE;
  if (HAL_DSI_ConfigAdaptedCommandMode(&hdsi, &CmdCfg) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_DSI_SetGenericVCID(&hdsi, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DSIHOST_Init 2 */

  /* USER CODE END DSIHOST_Init 2 */

}

/**
  * @brief JPEG Initialization Function
  * @param None
  * @retval None
  */
static void MX_JPEG_Init(void)
{

  /* USER CODE BEGIN JPEG_Init 0 */

  /* USER CODE END JPEG_Init 0 */

  /* USER CODE BEGIN JPEG_Init 1 */

  /* USER CODE END JPEG_Init 1 */
  hjpeg.Instance = JPEG;
  if (HAL_JPEG_Init(&hjpeg) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN JPEG_Init 2 */

  /* USER CODE END JPEG_Init 2 */

}

/**
  * @brief LTDC Initialization Function
  * @param None
  * @retval None
  */
static void MX_LTDC_Init(void)
{

  /* USER CODE BEGIN LTDC_Init 0 */

  /* USER CODE END LTDC_Init 0 */

  LTDC_LayerCfgTypeDef pLayerCfg = {0};

  /* USER CODE BEGIN LTDC_Init 1 */

  /* USER CODE END LTDC_Init 1 */
  hltdc.Instance = LTDC;
  hltdc.Init.HSPolarity = LTDC_HSPOLARITY_AH;
  hltdc.Init.VSPolarity = LTDC_VSPOLARITY_AH;
  hltdc.Init.DEPolarity = LTDC_DEPOLARITY_AL;
  hltdc.Init.PCPolarity = LTDC_PCPOLARITY_IPC;
  hltdc.Init.HorizontalSync = 0;
  hltdc.Init.VerticalSync = 0;
  hltdc.Init.AccumulatedHBP = 2;
  hltdc.Init.AccumulatedVBP = 2;
  hltdc.Init.AccumulatedActiveW = 402;
  hltdc.Init.AccumulatedActiveH = 482;
  hltdc.Init.TotalWidth = 403;
  hltdc.Init.TotalHeigh = 483;
  hltdc.Init.Backcolor.Blue = 0;
  hltdc.Init.Backcolor.Green = 0;
  hltdc.Init.Backcolor.Red = 0;
  if (HAL_LTDC_Init(&hltdc) != HAL_OK)
  {
    Error_Handler();
  }
  pLayerCfg.WindowX0 = 0;
  pLayerCfg.WindowX1 = 400;
  pLayerCfg.WindowY0 = 0;
  pLayerCfg.WindowY1 = 480;
  pLayerCfg.PixelFormat = LTDC_PIXEL_FORMAT_RGB888;
  pLayerCfg.Alpha = 255;
  pLayerCfg.Alpha0 = 0;
  pLayerCfg.BlendingFactor1 = LTDC_BLENDING_FACTOR1_PAxCA;
  pLayerCfg.BlendingFactor2 = LTDC_BLENDING_FACTOR2_PAxCA;
  pLayerCfg.FBStartAdress = 0xD0000000;
  pLayerCfg.ImageWidth = 400;
  pLayerCfg.ImageHeight = 480;
  pLayerCfg.Backcolor.Blue = 0;
  pLayerCfg.Backcolor.Green = 0;
  pLayerCfg.Backcolor.Red = 0;
  if (HAL_LTDC_ConfigLayer(&hltdc, &pLayerCfg, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LTDC_Init 2 */
  
      /* Configure DSI PHY HS2LP and LP2HS timings */
    
  __HAL_LTDC_DISABLE(&hltdc);
  DSI_LPCmdTypeDef LPCmd;
  
  HAL_DSI_Start(&hdsi);

  /* Configure the audio driver */
  IOCtx.Address     = 0;
  IOCtx.GetTick     = BSP_GetTick;
  IOCtx.WriteReg    = DSI_IO_Write;
  IOCtx.ReadReg     = DSI_IO_Read;
  OTM8009A_RegisterBusIO(&OTM8009AObj, &IOCtx);

  OTM8009A_Init(&OTM8009AObj ,OTM8009A_FORMAT_RGB888, OTM8009A_ORIENTATION_LANDSCAPE);
  HAL_DSI_ShortWrite(&hdsi, 0, DSI_DCS_SHORT_PKT_WRITE_P1, OTM8009A_CMD_DISPOFF, 0x00);

  LPCmd.LPGenShortWriteNoP = DSI_LP_GSW0P_DISABLE;
  LPCmd.LPGenShortWriteOneP = DSI_LP_GSW1P_DISABLE;
  LPCmd.LPGenShortWriteTwoP = DSI_LP_GSW2P_DISABLE;
  LPCmd.LPGenShortReadNoP = DSI_LP_GSR0P_DISABLE;
  LPCmd.LPGenShortReadOneP = DSI_LP_GSR1P_DISABLE;
  LPCmd.LPGenShortReadTwoP = DSI_LP_GSR2P_DISABLE;
  LPCmd.LPGenLongWrite = DSI_LP_GLW_DISABLE;
  LPCmd.LPDcsShortWriteNoP = DSI_LP_DSW0P_DISABLE;
  LPCmd.LPDcsShortWriteOneP = DSI_LP_DSW1P_DISABLE;
  LPCmd.LPDcsShortReadNoP = DSI_LP_DSR0P_DISABLE;
  LPCmd.LPDcsLongWrite = DSI_LP_DLW_DISABLE;
  HAL_DSI_ConfigCommand(&hdsi, &LPCmd);

  HAL_LTDC_SetPitch(&hltdc, 800, 0);
  __HAL_LTDC_ENABLE(&hltdc);
  /* USER CODE END LTDC_Init 2 */

}

/**
  * @brief QUADSPI Initialization Function
  * @param None
  * @retval None
  */
static void MX_QUADSPI_Init(void)
{

  /* USER CODE BEGIN QUADSPI_Init 0 */

  /* USER CODE END QUADSPI_Init 0 */

  /* USER CODE BEGIN QUADSPI_Init 1 */

  /* USER CODE END QUADSPI_Init 1 */
  /* QUADSPI parameter configuration*/
  hqspi.Instance = QUADSPI;
  hqspi.Init.ClockPrescaler = 3;
  hqspi.Init.FifoThreshold = 1;
  hqspi.Init.SampleShifting = QSPI_SAMPLE_SHIFTING_NONE;
  hqspi.Init.FlashSize = 1;
  hqspi.Init.ChipSelectHighTime = QSPI_CS_HIGH_TIME_1_CYCLE;
  hqspi.Init.ClockMode = QSPI_CLOCK_MODE_0;
  hqspi.Init.DualFlash = QSPI_DUALFLASH_ENABLE;
  if (HAL_QSPI_Init(&hqspi) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN QUADSPI_Init 2 */
  BSP_QSPI_Init_t init ;
  init.InterfaceMode=MT25TL01G_QPI_MODE;
  init.TransferRate= MT25TL01G_DTR_TRANSFER ;
  init.DualFlashMode= MT25TL01G_DUALFLASH_ENABLE;
  if (BSP_QSPI_Init(0,&init) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }
  if (BSP_QSPI_EnableMemoryMappedMode(0) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }
  /* USER CODE END QUADSPI_Init 2 */

}

/**
  * Enable MDMA controller clock
  */
static void MX_MDMA_Init(void)
{

  /* MDMA controller clock enable */
  __HAL_RCC_MDMA_CLK_ENABLE();
  /* Local variables */

  /* MDMA interrupt initialization */
  /* MDMA_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(MDMA_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(MDMA_IRQn);

}

/* FMC initialization function */
void MX_FMC_Init(void)
{

  /* USER CODE BEGIN FMC_Init 0 */

  /* USER CODE END FMC_Init 0 */

  FMC_SDRAM_TimingTypeDef SdramTiming = {0};

  /* USER CODE BEGIN FMC_Init 1 */
  FMC_Bank1_R->BTCR[0] &= ~FMC_BCRx_MBKEN;
  /* USER CODE END FMC_Init 1 */

  /** Perform the SDRAM1 memory initialization sequence
  */
  hsdram1.Instance = FMC_SDRAM_DEVICE;
  /* hsdram1.Init */
  hsdram1.Init.SDBank = FMC_SDRAM_BANK2;
  hsdram1.Init.ColumnBitsNumber = FMC_SDRAM_COLUMN_BITS_NUM_9;
  hsdram1.Init.RowBitsNumber = FMC_SDRAM_ROW_BITS_NUM_12;
  hsdram1.Init.MemoryDataWidth = FMC_SDRAM_MEM_BUS_WIDTH_32;
  hsdram1.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
  hsdram1.Init.CASLatency = FMC_SDRAM_CAS_LATENCY_3;
  hsdram1.Init.WriteProtection = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
  hsdram1.Init.SDClockPeriod = FMC_SDRAM_CLOCK_PERIOD_2;
  hsdram1.Init.ReadBurst = FMC_SDRAM_RBURST_ENABLE;
  hsdram1.Init.ReadPipeDelay = FMC_SDRAM_RPIPE_DELAY_0;
  /* SdramTiming */
  SdramTiming.LoadToActiveDelay = 2;
  SdramTiming.ExitSelfRefreshDelay = 7;
  SdramTiming.SelfRefreshTime = 4;
  SdramTiming.RowCycleDelay = 7;
  SdramTiming.WriteRecoveryTime = 3;
  SdramTiming.RPDelay = 2;
  SdramTiming.RCDDelay = 2;

  if (HAL_SDRAM_Init(&hsdram1, &SdramTiming) != HAL_OK)
  {
    Error_Handler( );
  }

  /* USER CODE BEGIN FMC_Init 2 */
  BSP_SDRAM_DeInit(0);
  if(BSP_SDRAM_Init(0) != BSP_ERROR_NONE)
  {
    Error_Handler();
  }
  /* USER CODE END FMC_Init 2 */
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* LED1 (PI12) and LED2 (PI13) — output, initially off */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin | LED2_Pin, GPIO_PIN_RESET);

  GPIO_InitTypeDef LED_InitStruct = {0};
  LED_InitStruct.Pin   = LED1_Pin | LED2_Pin;
  LED_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  LED_InitStruct.Pull  = GPIO_NOPULL;
  LED_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOI, &LED_InitStruct);
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOI_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOJ_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOJ, LCD_BL_Pin|FRAME_RATE_Pin|RENDER_TIME_Pin|VSYNC_FREQ_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(MCU_ACTIVE_GPIO_Port, MCU_ACTIVE_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LCD_BL_Pin FRAME_RATE_Pin RENDER_TIME_Pin VSYNC_FREQ_Pin */
  GPIO_InitStruct.Pin = LCD_BL_Pin|FRAME_RATE_Pin|RENDER_TIME_Pin|VSYNC_FREQ_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOJ, &GPIO_InitStruct);

  /*Configure GPIO pin : PA8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_RESET_Pin */
  GPIO_InitStruct.Pin = LCD_RESET_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(LCD_RESET_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : MCU_ACTIVE_Pin */
  GPIO_InitStruct.Pin = MCU_ACTIVE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(MCU_ACTIVE_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
static void MX_UART8_Init(void)
{
  huart8.Instance            = UART8;
  huart8.Init.BaudRate       = 921600;
  huart8.Init.WordLength     = UART_WORDLENGTH_8B;
  huart8.Init.StopBits       = UART_STOPBITS_1;
  huart8.Init.Parity         = UART_PARITY_NONE;
  huart8.Init.Mode           = UART_MODE_TX_RX;
  huart8.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
  huart8.Init.OverSampling   = UART_OVERSAMPLING_16;
  huart8.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart8.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart8.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart8) != HAL_OK)
  {
    Error_Handler();
  }
}

/* ── Helper: route one complete ASCII line received from NORA ────────────
 *
 * Called by UARTReceiveTask each time a '\n'-terminated line is complete.
 * Routes by prefix:
 *   "RESULT..." -> xBleQueue  (Hebrew translation -- unchanged pipeline)
 *   "TRACK:..."  -> xMusicQueue MSG_TRACK
 *   "THUMB:..."  -> sets s_thumbMode / s_thumbSize for binary receive
 *   "ERROR:..."  -> xMusicQueue MSG_ERROR
 *   anything else -> ignored
 * ──────────────────────────────────────────────────────────────────────── */
static void routeAsciiMessage(const char *msg, uint16_t len)
{
    LOG("[ROUTE] line: \"%s\" (%u bytes)\n", msg, (unsigned)len);
    BLE_UART_HistPush(msg);

    if (strncmp(msg, "RESULT:", 7) == 0)
    {
        /* Existing Hebrew pipeline -- unchanged. */
        char bleMsg[BLE_MSG_LEN];
        strncpy(bleMsg, msg, BLE_MSG_LEN - 1u);
        bleMsg[BLE_MSG_LEN - 1u] = '\0';
        if (xQueueSend(xBleQueue, bleMsg, 0) != pdTRUE)
            LOG("[WARN] xBleQueue full -- RESULT dropped!\n");
    }
    else if (strncmp(msg, "TRACK:", 6) == 0)
    {
        music_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type = MSG_TRACK;

        /* Parse: TRACK:<title>|<artist>|<videoId> */
        const char *p    = msg + 6;
        const char *sep1 = strchr(p, '|');
        const char *sep2 = sep1 ? strchr(sep1 + 1u, '|') : NULL;

        if (sep1 != NULL)
        {
            uint32_t tLen = (uint32_t)(sep1 - p);
            if (tLen >= sizeof(m.track.title)) tLen = sizeof(m.track.title) - 1u;
            memcpy(m.track.title, p, tLen);
            m.track.title[tLen] = '\0';

            if (sep2 != NULL)
            {
                uint32_t aLen = (uint32_t)(sep2 - sep1 - 1u);
                if (aLen >= sizeof(m.track.artist)) aLen = sizeof(m.track.artist) - 1u;
                memcpy(m.track.artist, sep1 + 1u, aLen);
                m.track.artist[aLen] = '\0';
                strncpy(m.track.videoId, sep2 + 1u, sizeof(m.track.videoId) - 1u);
            }
        }
        else
        {
            strncpy(m.track.title, p, sizeof(m.track.title) - 1u);
        }

        g_t_track = DWT_SNAP();
        LOG("[1] YouTube track received: \"%s\" | \"%s\" | videoId=%s\n",
            m.track.title, m.track.artist, m.track.videoId);
        LOG("[2] PC player notified (NORA HTTP POST)\n");

        if (xQueueSend(xMusicQueue, &m, 0) != pdTRUE)
            LOG("[WARN] xMusicQueue full -- TRACK dropped\n");
    }
    else if (strncmp(msg, "THUMB:", 6) == 0)
    {
        s_thumbSize     = (uint32_t)atoi(msg + 6);
        s_thumbReceived = 0u;
        s_thumbMode     = true;
    }
    else if (strncmp(msg, "ERROR:", 6) == 0)
    {
        LOG("[ERROR] NORA error: \"%s\"\n", msg + 6);
        music_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type = MSG_ERROR;
        strncpy(m.error.reason, msg + 6, sizeof(m.error.reason) - 1u);
        if (xQueueSend(xMusicQueue, &m, 0) != pdTRUE)
            LOG("[WARN] xMusicQueue full -- ERROR dropped\n");
    }
}

/**
 * UARTReceiveTask -- processes raw DMA snapshots, routes by message type.
 *
 * ASCII messages (RESULT/TRACK/ERROR) are newline-terminated and accumulated
 * in accum[] across DMA bursts, then dispatched via routeAsciiMessage().
 *
 * Binary THUMB: data is accumulated directly into s_jpegInBuf[] byte by byte
 * using the same DMA+IDLE pipeline -- no separate HAL_UART_Receive_DMA call.
 * This avoids timing races: the ISR always restarts idle-DMA immediately, so
 * binary bytes flow through xRawBleQueue in 128-byte chunks just like text.
 *
 * After the "THUMB:<size>\n" header line is routed, s_thumbMode is set.
 * All subsequent bytes from xRawBleQueue are copied into s_jpegInBuf until
 * s_thumbReceived == s_thumbSize, then MSG_THUMB is posted to xMusicQueue.
 *
 * Any bytes in the same DMA burst that follow a '\n' are processed correctly
 * because the inner while-loop continues after handling the newline.
 */
static void UARTReceiveTask(void *argument)
{
    (void)argument;

    /* Enable DWT cycle counter (CM7 @ 480 MHz). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    LOG("\n========================================\n"
        "[UART] UARTReceiveTask started -- DMA+IDLE mode\n"
        "       UART8 |  921600 8N1 | DMA1 Stream0\n"
        "       DWT cycle counter enabled (480 MHz)\n"
        "========================================\n\n");

    BLE_UART_StartDMA();
    LOG("[UART] DMA armed -- listening on UART8\n");

    static char accum[UART_ACCUM_SIZE];
    uint16_t    accumLen = 0u;
    accum[0] = '\0';

    BleRawMsg_t raw;

    for (;;)
    {
        /* Block until the ISR posts a raw DMA burst (or 500 ms timeout). */
        BaseType_t got = xQueueReceive(xRawBleQueue, &raw, pdMS_TO_TICKS(500));

        if (got != pdTRUE)
        {
            /* 500 ms with no bytes: flush any partial ASCII fragment. */
            if (accumLen > 0u && !s_thumbMode)
            {
                LOG("[UART] Timeout flush (%u bytes): \"%s\"\n",
                    (unsigned)accumLen, accum);
                routeAsciiMessage(accum, accumLen);
                accumLen = 0u;
                accum[0] = '\0';
            }
            else
            {
                LOG("[UART] heartbeat -- idle (ISR count=%lu)\n", g_uartIsrCount);
            }
            continue;
        }

        TLOG("3 xRawBleQueue_recv len=%u  t=%lu us", raw.len, T_US());
        LOG("[UART] burst: %u bytes  [%02X %02X %02X %02X]\n",
            raw.len,
            raw.len > 0u ? raw.data[0] : 0u,
            raw.len > 1u ? raw.data[1] : 0u,
            raw.len > 2u ? raw.data[2] : 0u,
            raw.len > 3u ? raw.data[3] : 0u);

        /* Process every byte in this DMA burst. */
        uint16_t i = 0u;
        while (i < raw.len)
        {
            if (s_thumbMode)
            {
                /* ── Binary JPEG receive ─────────────────────────────── */
                uint32_t toCopy    = (uint32_t)(raw.len - i);
                uint32_t remaining = s_thumbSize - s_thumbReceived;
                if (toCopy > remaining) toCopy = remaining;

                memcpy(s_jpegInBuf + s_thumbReceived, raw.data + i, toCopy);
                s_thumbReceived += toCopy;
                i               += (uint16_t)toCopy;

                /* Point 4: log every 10% of THUMB accumulation */
                {
                    uint32_t pct      = (s_thumbReceived * 100u) / s_thumbSize;
                    uint32_t prev_pct = ((s_thumbReceived - toCopy) * 100u) / s_thumbSize;
                    if (pct / 10u != prev_pct / 10u)
                        TLOG("4 THUMB_accum %lu%%  %lu/%lu B  t=%lu us",
                             pct, s_thumbReceived, s_thumbSize, T_US());
                }

                if (s_thumbReceived >= s_thumbSize)
                {
                    g_t_thumb = DWT_SNAP();
                    LOG("[3] Thumbnail JPEG received: %lu bytes  [transfer: %lu ms / %lu Kcycles from track]\n",
                        s_thumbSize,
                        DWT_MS(g_t_thumb - g_t_track),
                        (g_t_thumb - g_t_track) / 1000UL);
                    s_thumbMode = false;

                    /* Clean D-Cache so JpegDisplayTask sees fresh data. */
                    SCB_CleanDCache_by_Addr(
                        (uint32_t *)s_jpegInBuf,
                        (int32_t)((s_thumbSize + 31u) & ~31u));

                    music_msg_t m;
                    m.type       = MSG_THUMB;
                    m.thumb.data = s_jpegInBuf;
                    m.thumb.size = s_thumbSize;

                    if (xQueueSend(xMusicQueue, &m, 0) != pdTRUE)
                        LOG("[UART] WARNING: xMusicQueue full -- THUMB dropped\n");
                    else
                        TLOG("5 MSG_THUMB_sent size=%lu  t=%lu us", s_thumbSize, T_US());
                }
                /* Continue the while loop -- remaining bytes (if any) are
                   ASCII from the next message. */
            }
            else
            {
                /* ── ASCII accumulation ──────────────────────────────── */
                uint8_t b = raw.data[i++];

                if (b == (uint8_t)'\r') continue;

                if (b == (uint8_t)'\n')
                {
                    if (accumLen > 0u)
                    {
                        routeAsciiMessage(accum, accumLen);
                        accumLen = 0u;
                        accum[0] = '\0';
                        /* s_thumbMode may now be true -- the next iteration
                           of the while loop will handle binary bytes. */
                    }
                    continue;
                }

                if (accumLen < UART_ACCUM_SIZE - 1u)
                {
                    accum[accumLen++] = (char)b;
                    accum[accumLen]   = '\0';
                }
                else
                {
                    /* Buffer full: flush and start fresh with this byte. */
                    LOG("[UART] Buffer full -- flushing\n");
                    routeAsciiMessage(accum, accumLen);
                    accumLen    = 0u;
                    accum[0]    = (char)b;
                    accumLen    = 1u;
                    accum[1]    = '\0';
                }
            }
        }
    }
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_TouchGFX_Task */
/**
  * @brief  Function implementing the TouchGFXTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_TouchGFX_Task */
__weak void TouchGFX_Task(void *argument)
{
  /* USER CODE BEGIN 5 */
  /* Infinite loop */
  for(;;)
  {
    osDelay(1);
  }
  /* USER CODE END 5 */
}

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x90000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_256MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER1;
  MPU_InitStruct.Size = MPU_REGION_SIZE_128MB;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER2;
  MPU_InitStruct.BaseAddress = 0xD0000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32MB;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER3;
  MPU_InitStruct.BaseAddress = 0x24000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER4;
  MPU_InitStruct.BaseAddress = 0x10000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_256KB;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Number = MPU_REGION_NUMBER5;
  MPU_InitStruct.BaseAddress = 0x10040000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_32KB;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
