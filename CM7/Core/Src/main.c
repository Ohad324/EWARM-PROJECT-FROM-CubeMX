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
#include "audio_sd.h"            /* SD card WAV recording — AudioSD_Init()           */
#include "voice_recorder.h"      /* VoiceRec_ButtonInit(), button ISR, RTT log       */
#include "command_handler.h"     /* voice CMD: receiver — CommandHandler_Init()       */
#include "log_mutex.h"           /* LOG() macro — outputs to SEGGER RTT */
#include "music_display_task.h"  /* Music_Init(), xMusicQueue, music_msg_t */
#include "cmsis_os2.h"           /* osKernelGetTickCount() */
#include "timing_log.h"          /* TLOG(), T_US() — RTT timing instrumentation */
#include "rtos_trace.h"          /* RtosTrace_Init(), RtosTrace_DrainTask()     */
#include "usb_msc.h"             /* USB_MSC_TaskEntry() — boot-time format recovery */
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

DFSDM_Filter_HandleTypeDef hdfsdm1_filter0;
DFSDM_Channel_HandleTypeDef hdfsdm1_channel0;  /* CH0 = SAI4 MicPair1 D1 (rising edge test — was CH1) */

DMA2D_HandleTypeDef hdma2d;

DSI_HandleTypeDef hdsi;

JPEG_HandleTypeDef hjpeg;
MDMA_HandleTypeDef hmdma_jpeg_infifo_th;
MDMA_HandleTypeDef hmdma_jpeg_outfifo_th;

LTDC_HandleTypeDef hltdc;

SDRAM_HandleTypeDef hsdram1;

QSPI_HandleTypeDef hqspi;

SAI_HandleTypeDef hsai_BlockA4;

UART_HandleTypeDef huart8;

/* Definitions for TouchGFXTask */
osThreadId_t TouchGFXTaskHandle;
const osThreadAttr_t TouchGFXTask_attributes = {
  .name = "TouchGFXTask",
  .stack_size = 5120 * 4,   /* was 3048 — enlarged for JPEG decode stack (Music_Poll) */
  .priority = (osPriority_t) osPriorityBelowNormal,  /* was Normal(24) → BelowNormal(16) < SDWriteTask(20) */
};
/* Definitions for videoTask */
osThreadId_t videoTaskHandle;
const osThreadAttr_t videoTask_attributes = {
  .name = "videoTask",
  .stack_size = 1000 * 4,
  .priority = (osPriority_t) osPriorityLow,
};
/* USER CODE BEGIN PV */
OTM8009A_Object_t OTM8009AObj;
OTM8009A_IO_t IOCtx;
UART_HandleTypeDef huart8;

/* BLE UART queue — receives messages from UARTReceiveTask, consumed by Model::tick() */
QueueHandle_t xBleQueue;

/* ── DFSDM debug snapshot — readable via JLink mem32 without RTT viewer ────
 * Captured immediately after SPICKSEL=11 write in MX_DFSDM1_Init().
 * Expected value: 0x0000008D (CHEN=1, SPICKSEL=11, SITP=01).
 * If 0x00000085: SPICKSEL write did not apply (CHEN was not cleared first).
 * If 0x00000000: DFSDM clock not enabled (RCC gate missing).              */
volatile uint32_t g_dbg_ch3_chcfgr1  = 0xDEADBEEFu;  /* Ch3 CHCFGR1 after SPICKSEL=11 */
volatile uint32_t g_dbg_ch0_chcfgr1  = 0xDEADBEEFu;  /* Ch0 CHCFGR1 (DFSDMEN+CKOUTDIV) */
volatile uint32_t g_dbg_flt0_fltcr1  = 0xDEADBEEFu;  /* Filter0 FLTCR1 (DFEN+CH3+Sinc3) */
volatile uint32_t g_dbg_sai4_cr1     = 0xDEADBEEFu;  /* SAI4 CR1 (MCKDIV+SAIEN+PDM) */
volatile uint32_t g_dbg_sai4_pdmcr   = 0xDEADBEEFu;  /* SAI4 PDMCR (PDMEN+CKEN1) */

/* ── LIVE debug snapshot — captured at VoiceRecTask recording-start time ────
 * Updated on each recording trigger (overwritten each time).
 * g_dbg_live_sai4_cr1: SAI4 CR1 right after __HAL_SAI_ENABLE → expect bit16=1 (SAIEN).
 * g_dbg_live_fltisr:   DFSDM Filter0 FLTISR after DMA start → bit19=CKABF[3] (1=no clock).
 * g_dbg_live_hal_ok:   HAL_DFSDM_FilterRegularStart_DMA return: 0x00000000=OK, 0xFFFFFFFF=FAIL.
 * g_dbg_live_fltcr1:   FLTCR1 right before DMA start (after RDMAEN force).            */
volatile uint32_t g_dbg_live_sai4_cr1 = 0xDEADBEEFu;
volatile uint32_t g_dbg_live_fltisr   = 0xDEADBEEFu;
volatile uint32_t g_dbg_live_hal_ok   = 0xDEADBEEFu;
volatile uint32_t g_dbg_live_fltcr1   = 0xDEADBEEFu;

/* Voice recorder queue — VoiceRecTask signals SDWriteTask when 5s recording is done */
static QueueHandle_t xVoiceQueue;


/* ── DWT cycle-counter helpers (CM7 @ 400 MHz) ─────────────────────────── */
#define DWT_CYCLES_PER_MS  400000UL
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
    .priority   = (osPriority_t) 26,
};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_MDMA_Init(void);
static void MX_QUADSPI_Init(void);
static void MX_FMC_Init(void);
static void MX_DMA2D_Init(void);
static void MX_DSIHOST_DSI_Init(void);
static void MX_LTDC_Init(void);
static void MX_CRC_Init(void);
static void MX_JPEG_Init(void);
static void MX_SAI4_Init(void);
static void MX_UART8_Init(void);
static void MX_DFSDM1_Init(void);
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

  /* Configure The Vector Table address — CM7 vectors live in Bank 1 per ICF */
  SCB->VTOR = 0x08000000;

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
  MX_QUADSPI_Init();
  MX_FMC_Init();
  MX_DMA2D_Init();
  MX_DSIHOST_DSI_Init();
  MX_LTDC_Init();
  MX_CRC_Init();
  MX_JPEG_Init();
  MX_LIBJPEG_Init();
  MX_SAI4_Init();
  MX_UART8_Init();
  MX_DFSDM1_Init();
  MX_TouchGFX_Init();
  /* Call PreOsInit function */
  MX_TouchGFX_PreOSInit();
  /* USER CODE BEGIN 2 */
  /* MX_UART8_Init() already called above in the CubeMX init sequence — removed duplicate. */
  /* Attach DMA1 Stream0 to huart8 and create xRawBleQueue / xBleHistMutex.
     Must run after MX_UART8_Init() (UART handle ready) and before
     osKernelStart() (RTOS objects created here).
     The actual HAL_UARTEx_ReceiveToIdle_DMA() call happens inside
     UARTReceiveTask so the FreeRTOS ISR infrastructure is live first. */
  BLE_UART_Init();
  /* Full voice recorder init: button EXTI + DFSDM + DMA + RTOS objects.
     audio_rec.c is excluded from build so no DMA/GPIO conflict. */
  VoiceRec_Init();
  // AudioRec_Init();   /* DISABLED — superseded by voice_recorder.c */
  AudioSD_Init();    /* SDMMC1 init + FatFS mount — non-fatal if card absent       */

  CommandHandler_Init(); /* create CMD: message queue                              */
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
  /* Voice recorder handoff queue: depth 1, VoiceRecTask → SDWriteTask */
  xVoiceQueue = xQueueCreate(1, sizeof(uint32_t));
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of TouchGFXTask */
  TouchGFXTaskHandle = osThreadNew(TouchGFX_Task, NULL, &TouchGFXTask_attributes);

  /* videoTask removed — MJPEG decoder not active in this project.
   * videoController object stays in TouchGFXGeneratedHAL.cpp so video
   * widgets compile; they just do not decode frames. Re-enable if needed. */
  /* videoTaskHandle = osThreadNew(videoTaskFunc, NULL, &videoTask_attributes); */

  /* USER CODE BEGIN RTOS_THREADS */
  osThreadNew(UARTReceiveTask, NULL, &uartReceiveTask_attributes);
  /* AudioRecTask: waits for button press, records mic via DFSDM DMA, sends PCM over UART8.
     Stack 4096 bytes: needs ~1 KB for pcm16[] static buffer + FreeRTOS overhead.
     Priority normal: same as TouchGFX — audio send is bursty, not latency-critical. */
  /* AudioRec DISABLED — DFSDM/GPIO conflict crashes thumbnail pipeline.
     Re-enable only after thumbnail display is stable. */
  // xTaskCreate(AudioRec_TaskEntry, "AudioRec", 4096u, NULL, osPriorityNormal, NULL);
  /* CommandHandler: receives CMD: messages from NORA, dispatches to screen.
     Stack 1024 words.  Priority below normal: display updates are not time-critical. */
  xTaskCreate(CommandHandler_TaskEntry, "VoiceCMDhandler", 1024u, NULL,
              osPriorityBelowNormal, NULL);
  /* Voice recorder pipeline — prio/stack per CLAUDE.md task map */
  xTaskCreate(VoiceRecTask,  "VoiceRecTask",  2048u, xVoiceQueue, 32u, &voiceRecTaskHandle);
  xTaskCreate(SDWriteTask,   "SDWriteTask",   2048u, xVoiceQueue, 20u, NULL);
  xTaskCreate(RTTLogTask,    "RTTLogTask",     256u, NULL,        1u, NULL);
  /* HealthMonTask removed — health logged by SDWriteTask after each f_close() */
  /* RTOS trace drain task — prio 1 (lowest app priority), 512-word stack */
  RtosTrace_Init();
  xTaskCreate(RtosTrace_DrainTask, "rtos_trace", 512u, NULL, 1u, NULL);
  /* USB MSC format recovery — hold blue button 4s at boot → board exposes
   * SD as USB MSC on CN1 → diskpart formats as exFAT (MBR+partition) →
   * board resets. FF_MULTI_PARTITION=1 then mounts the result. */
  xTaskCreate(USB_MSC_TaskEntry, "UsbMscTask", 512u, NULL, 2u, NULL);

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
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1); /* VOS1 sufficient for 400 MHz */

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  /* ── Step 3: Enable PLL1 (VOS1 and flash wait states set above) ──
   * Safety sequence: VOS1 → wait VOSRDY → Flash latency → PLL enable
   * HSE=25 MHz / PLLM=2 = 12.5 MHz → ×PLLN=64 = 800 MHz VCO
   * PLLP=2 → SYSCLK=400 MHz, PLLQ=4 → 200 MHz (SDMMC/Audio) */
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 64;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;  /* 8–16 MHz range for 12.5 MHz input */
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;  /* Wide VCO for 800 MHz */
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
   * Bus safety: AHB max=240 MHz, APBx max=120 MHz
   * SYSCLK=400/1=400, AHB=400/2=200, APBx=200/2=100 — all within limits */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;    /* 400 MHz */
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;      /* 200 MHz */
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;     /* 100 MHz */
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;     /* 100 MHz */
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;     /* 100 MHz */
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;     /* 100 MHz */

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
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
  * @brief DFSDM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_DFSDM1_Init(void)
{

  /* USER CODE BEGIN DFSDM1_Init 0 */

  /* USER CODE END DFSDM1_Init 0 */

  /* USER CODE BEGIN DFSDM1_Init 1 */

  /* USER CODE END DFSDM1_Init 1 */
  /* USER CODE BEGIN DFSDM1_Init 2 */
  /* HAL rule: Channel MUST be initialized before Filter (one call each).
   * SPI_CLOCK_INTERNAL = SAI4 provides the PDM clock internally (~2.048 MHz).
   * CKOUT pin (PD3) is not used — OutputClock disabled.
   * Clock math: 2.048 MHz / OSR=125 = 16,384 Hz ≈ 16 kHz PCM ✓
   * LR=GND on MP34DT05-A → data on falling edge → SPI_FALLING.
   * RightBitShift=5: Sinc3 peak = 125³ = ~21-bit → shift 5 → 16-bit output. */

  /* ── DFSDM CKOUT + global enable — ORDER IS CRITICAL (RM0399 §30.4.2) ──────
   * RM0399: "CKOUTDIV bits are writable only when DFSDMEN=0."
   * RM0399: "CHEN is writable only when DFSDMEN=1."
   * Therefore the sequence MUST be:
   *   1. CKOUTDIV  (while DFSDMEN=0 — HW ignores write if DFSDMEN=1 already)
   *   2. DFSDMEN=1 (locks in CKOUTDIV, enables peripheral)
   *   3. HAL_DFSDM_ChannelInit (writes CHEN=1, valid now that DFSDMEN=1)
   *
   * CKOUT = APB2 / ((CKOUTDIV+1) × 2) = 100 MHz / ((24+1)×2) = 2.0 MHz
   * Sinc3 OSR=125: PCM = 2.0 MHz / 125 = 16,000 Hz.
   * SAI4 AudioFrequency=SAI_AUDIO_FREQUENCY_16K → MCKDIV=11 → CK1≈2.24 MHz ≈ CKOUT. */

  /* ── RCC clock enable — DUAL GATE for STM32H747 dual-core ──────────────────
   * On H747, two independent RCC gates must be set for CM7 to access DFSDM1:
   *   __HAL_RCC_DFSDM1_CLK_ENABLE()    → RCC->APB2ENR    (shared bus clock)
   *   __HAL_RCC_C1_DFSDM1_CLK_ENABLE() → RCC_C1->APB2ENR (CM7-core gate)
   * Using only the shared gate leaves DMA reachable but CM7 register writes
   * silently no-op — the C1 gate is required for reliable CM7 register access. */
  __HAL_RCC_DFSDM1_CLK_ENABLE();      /* shared APB2 bus clock — enables DMA path */
  __HAL_RCC_C1_DFSDM1_CLK_ENABLE();  /* CM7 core gate — required for register access on H747 */

  DFSDM1_Channel0->CHCFGR1 |= (24u << DFSDM_CHCFGR1_CKOUTDIV_Pos);  /* STEP 1: CKOUTDIV while DFSDMEN=0 */
  DFSDM1_Channel0->CHCFGR1 |= DFSDM_CHCFGR1_DFSDMEN;                 /* STEP 2: enable — locks CKOUTDIV */

  /* ── Step 1: Channel 1 (SAI4 MicPair1 D1 — LR=GND, falling edge) ──────────
   * Root cause analysis (2026-04-13):
   *   CH3 (DFSDM1_DATIN3 = PC7) does NOT connect to SAI4 MicPair1 D1 in silicon.
   *   The SAI4→DFSDM internal bridge routes:
   *     MicPair1 D1 (falling edge, LR=GND mic on PC1) → DFSDM1_Channel1
   *     MicPair1 D0 (rising edge, empty)               → DFSDM1_Channel0
   *     MicPair2 D1 (falling edge, 2nd mic)            → DFSDM1_Channel3
   *   CH3 was sampling PC7 (DFSDM_DATIN3) which is pulled HIGH → constant all-1s.
   *   CH1 (falling edge) showed all-0s — testing CH0 (rising edge) per Gemini hypothesis. */
  hdfsdm1_channel0.Instance                        = DFSDM1_Channel0;
  hdfsdm1_channel0.Init.OutputClock.Activation     = DISABLE;  /* CKOUT unused — SAI4 is clock source */
  hdfsdm1_channel0.Init.OutputClock.Selection      = DFSDM_CHANNEL_OUTPUT_CLOCK_SYSTEM;
  hdfsdm1_channel0.Init.OutputClock.Divider        = 2u;       /* irrelevant when Activation=DISABLE */
  hdfsdm1_channel0.Init.Input.Multiplexer          = DFSDM_CHANNEL_EXTERNAL_INPUTS;
  hdfsdm1_channel0.Init.Input.DataPacking          = DFSDM_CHANNEL_STANDARD_MODE;
  hdfsdm1_channel0.Init.Input.Pins                 = DFSDM_CHANNEL_SAME_CHANNEL_PINS;
  hdfsdm1_channel0.Init.SerialInterface.Type       = DFSDM_CHANNEL_SPI_RISING;   /* rising edge test */
  hdfsdm1_channel0.Init.SerialInterface.SpiClock   = DFSDM_CHANNEL_SPI_CLOCK_INTERNAL;
  hdfsdm1_channel0.Init.Awd.FilterOrder            = DFSDM_CHANNEL_FASTSINC_ORDER;
  hdfsdm1_channel0.Init.Awd.Oversampling           = 10u;
  hdfsdm1_channel0.Init.Offset                     = 0;
  hdfsdm1_channel0.Init.RightBitShift              = 5u;
  if (HAL_DFSDM_ChannelInit(&hdfsdm1_channel0) != HAL_OK) { Error_Handler(); }

  /* ── Override SPICKSEL=11: route Channel 0 to SAI4 Block A internal bridge ─
   * HAL sets SPICKSEL=01 (DFSDM_CHANNEL_SPI_CLOCK_INTERNAL = CKOUT).
   * We need SPICKSEL=11 so CH0 receives clock+data from SAI4 bridge, not DATIN0 pin.
   * SPICKSEL is write-protected when CHEN=1 → clear CHEN, write, restore. */
  DFSDM1_Channel0->CHCFGR1 &= ~DFSDM_CHCFGR1_CHEN;
  DFSDM1_Channel0->CHCFGR1  = (DFSDM1_Channel0->CHCFGR1 & ~DFSDM_CHCFGR1_SPICKSEL_Msk)
                             | (DFSDM_CHCFGR1_SPICKSEL_0 | DFSDM_CHCFGR1_SPICKSEL_1); /* =11: SAI4-A → CH0 */
  DFSDM1_Channel0->CHCFGR1 |=  DFSDM_CHCFGR1_CHEN;

  /* ── Snapshot registers into globals — readable via JLink without RTT ── */
  g_dbg_ch3_chcfgr1 = DFSDM1_Channel0->CHCFGR1;   /* expect 0x0000008C (CH0 SPICKSEL=11, SITP=00 rising) */
  g_dbg_ch0_chcfgr1 = DFSDM1_Channel0->CHCFGR1;   /* expect 0x80180000 (DFSDMEN+CKOUTDIV=24) */

  /* ── Step 2: Filter 0 ── */
  hdfsdm1_filter0.Instance                          = DFSDM1_Filter0;
  hdfsdm1_filter0.Init.RegularParam.Trigger         = DFSDM_FILTER_SW_TRIGGER;
  hdfsdm1_filter0.Init.RegularParam.FastMode        = ENABLE;
  hdfsdm1_filter0.Init.RegularParam.DmaMode         = ENABLE;
  hdfsdm1_filter0.Init.FilterParam.SincOrder        = DFSDM_FILTER_SINC3_ORDER;
  hdfsdm1_filter0.Init.FilterParam.Oversampling     = 125u;
  hdfsdm1_filter0.Init.FilterParam.IntOversampling  = 1u;
  if (HAL_DFSDM_FilterInit(&hdfsdm1_filter0) != HAL_OK) { Error_Handler(); }

  /* ── Step 3: Assign channel to filter ── */
  if (HAL_DFSDM_FilterConfigRegChannel(&hdfsdm1_filter0, DFSDM_CHANNEL_0,
                                        DFSDM_CONTINUOUS_CONV_ON) != HAL_OK)
  { Error_Handler(); }

  g_dbg_flt0_fltcr1 = DFSDM1_Filter0->FLTCR1;     /* expect 0x23240001 */
  /* USER CODE END DFSDM1_Init 2 */

}

/**
  * @brief FMC Initialization Function — configures SDRAM bank 2 (IS42S32800J-6BLI)
  *        MUST be called before MX_DMA2D_Init / MX_LTDC_Init, as both write to
  *        the 0xD000'0000 SDRAM range.
  * @param None
  * @retval None
  */
static void MX_FMC_Init(void)
{
  /* USER CODE BEGIN FMC_Init 0 */
  /* USER CODE END FMC_Init 0 */

  FMC_SDRAM_TimingTypeDef SdramTiming = {0};

  /* USER CODE BEGIN FMC_Init 1 */
  FMC_Bank1_R->BTCR[0] &= ~FMC_BCRx_MBKEN;
  /* USER CODE END FMC_Init 1 */

  hsdram1.Instance = FMC_SDRAM_DEVICE;
  hsdram1.Init.SDBank             = FMC_SDRAM_BANK2;
  hsdram1.Init.ColumnBitsNumber   = FMC_SDRAM_COLUMN_BITS_NUM_9;
  hsdram1.Init.RowBitsNumber      = FMC_SDRAM_ROW_BITS_NUM_12;
  hsdram1.Init.MemoryDataWidth    = FMC_SDRAM_MEM_BUS_WIDTH_32;
  hsdram1.Init.InternalBankNumber = FMC_SDRAM_INTERN_BANKS_NUM_4;
  hsdram1.Init.CASLatency         = FMC_SDRAM_CAS_LATENCY_3;
  hsdram1.Init.WriteProtection    = FMC_SDRAM_WRITE_PROTECTION_DISABLE;
  hsdram1.Init.SDClockPeriod      = FMC_SDRAM_CLOCK_PERIOD_2;
  hsdram1.Init.ReadBurst          = FMC_SDRAM_RBURST_ENABLE;
  hsdram1.Init.ReadPipeDelay      = FMC_SDRAM_RPIPE_DELAY_0;

  SdramTiming.LoadToActiveDelay    = 2;
  SdramTiming.ExitSelfRefreshDelay = 7;
  SdramTiming.SelfRefreshTime      = 4;
  SdramTiming.RowCycleDelay        = 7;
  SdramTiming.WriteRecoveryTime    = 3;
  SdramTiming.RPDelay              = 2;
  SdramTiming.RCDDelay             = 2;

  if (HAL_SDRAM_Init(&hsdram1, &SdramTiming) != HAL_OK) { Error_Handler(); }

  /* USER CODE BEGIN FMC_Init 2 */
  BSP_SDRAM_DeInit(0);
  if (BSP_SDRAM_Init(0) != BSP_ERROR_NONE) { Error_Handler(); }
  /* USER CODE END FMC_Init 2 */
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
  * @brief SAI4 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SAI4_Init(void)
{

  /* USER CODE BEGIN SAI4_Init 0 */

  /* USER CODE END SAI4_Init 0 */

  /* USER CODE BEGIN SAI4_Init 1 */

  /* USER CODE END SAI4_Init 1 */
  hsai_BlockA4.Instance = SAI4_Block_A;
  hsai_BlockA4.Init.Protocol = SAI_FREE_PROTOCOL;
  hsai_BlockA4.Init.AudioMode = SAI_MODEMASTER_RX;
  hsai_BlockA4.Init.DataSize = SAI_DATASIZE_8;
  hsai_BlockA4.Init.FirstBit = SAI_FIRSTBIT_MSB;
  hsai_BlockA4.Init.ClockStrobing = SAI_CLOCKSTROBING_FALLINGEDGE;
  hsai_BlockA4.Init.Synchro = SAI_ASYNCHRONOUS;
  hsai_BlockA4.Init.OutputDrive = SAI_OUTPUTDRIVE_DISABLE;
  hsai_BlockA4.Init.NoDivider = SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockA4.Init.MckOverSampling = SAI_MCK_OVERSAMPLING_DISABLE;
  hsai_BlockA4.Init.FIFOThreshold = SAI_FIFOTHRESHOLD_EMPTY;
  hsai_BlockA4.Init.AudioFrequency = SAI_AUDIO_FREQUENCY_16K;  /* 16 kHz → MCKDIV=12 → CK1=2.048 MHz ≈ DFSDM CKOUT 2.0 MHz */
  hsai_BlockA4.Init.MonoStereoMode = SAI_STEREOMODE;
  hsai_BlockA4.Init.CompandingMode = SAI_NOCOMPANDING;
  hsai_BlockA4.Init.PdmInit.Activation = ENABLE;
  hsai_BlockA4.Init.PdmInit.MicPairsNbr = 1;
  hsai_BlockA4.Init.PdmInit.ClockEnable = SAI_PDM_CLOCK1_ENABLE;
  hsai_BlockA4.FrameInit.FrameLength = 8;
  hsai_BlockA4.FrameInit.ActiveFrameLength = 1;
  hsai_BlockA4.FrameInit.FSDefinition = SAI_FS_STARTFRAME;
  hsai_BlockA4.FrameInit.FSPolarity = SAI_FS_ACTIVE_LOW;
  hsai_BlockA4.FrameInit.FSOffset = SAI_FS_FIRSTBIT;
  hsai_BlockA4.SlotInit.FirstBitOffset = 0;
  hsai_BlockA4.SlotInit.SlotSize = SAI_SLOTSIZE_DATASIZE;
  hsai_BlockA4.SlotInit.SlotNumber = 1;
  hsai_BlockA4.SlotInit.SlotActive = SAI_SLOTACTIVE_0;  /* slot 0 active — SLOTR=0x00010000; SAI4 captures D1 on PC1 */
  if (HAL_SAI_Init(&hsai_BlockA4) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SAI4_Init 2 */

  /* ── Force MCKDIV=12 — HAL rounds down to 11 for PLL2P=49.14 MHz ──────────
   * HAL formula: MCKDIV = floor(49142857 / (16000 × 256 × 1)) = floor(12.0) = 11
   * CK1 at MCKDIV=11: 49142857 / 22 = 2.234 MHz — 11.7% faster than DFSDM CKOUT
   * CK1 at MCKDIV=12: 49142857 / 24 = 2.048 MHz — only 2.4% from CKOUT=2.0 MHz ✓
   * Write MCKDIV while SAIEN=0 (HAL_SAI_Init leaves SAIEN=0 until DMA start). */
  hsai_BlockA4.Instance->CR1 = (hsai_BlockA4.Instance->CR1 & ~SAI_xCR1_MCKDIV) |
                                (12u << SAI_xCR1_MCKDIV_Pos);

  g_dbg_sai4_cr1   = hsai_BlockA4.Instance->CR1;    /* expect 0x00C10040 (MCKDIV=12, SAIEN=0 yet) */
  g_dbg_sai4_pdmcr = SAI4->PDMCR;                   /* expect 0x00000101 (PDMEN+CKEN1) */

  /* USER CODE END SAI4_Init 2 */

}

/**
  * @brief UART8 Initialization Function
  * @param None
  * @retval None
  */
static void MX_UART8_Init(void)
{

  /* USER CODE BEGIN UART8_Init 0 */

  /* USER CODE END UART8_Init 0 */

  /* USER CODE BEGIN UART8_Init 1 */

  /* USER CODE END UART8_Init 1 */
  huart8.Instance = UART8;
  huart8.Init.BaudRate = 921600;
  huart8.Init.WordLength = UART_WORDLENGTH_8B;
  huart8.Init.StopBits = UART_STOPBITS_1;
  huart8.Init.Parity = UART_PARITY_NONE;
  huart8.Init.Mode = UART_MODE_TX_RX;
  huart8.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart8.Init.OverSampling = UART_OVERSAMPLING_16;
  huart8.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart8.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart8.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart8, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart8, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_EnableFifoMode(&huart8) != HAL_OK)  /* Enable 16-deep HW FIFO — prevents ORE at 921600 baud */
  {
    Error_Handler();
  }
  /* USER CODE BEGIN UART8_Init 2 */

  /* USER CODE END UART8_Init 2 */

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
  __HAL_RCC_GPIOI_CLK_ENABLE();   /* must be before HAL_GPIO_Init */
  HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin | LED2_Pin, GPIO_PIN_RESET);

  GPIO_InitTypeDef LED_InitStruct = {0};
  LED_InitStruct.Pin   = LED1_Pin | LED2_Pin;
  LED_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  LED_InitStruct.Pull  = GPIO_NOPULL;
  LED_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOI, &LED_InitStruct);
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOG_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOJ_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_RESET_GPIO_Port, LCD_RESET_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(VSYNC_FREQ_GPIO_Port, VSYNC_FREQ_Pin, GPIO_PIN_RESET);

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

  /*Configure GPIO pin : VSYNC_FREQ_Pin */
  GPIO_InitStruct.Pin = VSYNC_FREQ_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(VSYNC_FREQ_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

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
    RLOG("[ROUTE] line: \"%s\" (%u bytes)", msg, (unsigned)len);
    BLE_UART_HistPush(msg);

    if (strncmp(msg, "RESULT:", 7) == 0)
    {
        /* Existing Hebrew pipeline -- unchanged. */
        char bleMsg[BLE_MSG_LEN];
        strncpy(bleMsg, msg, BLE_MSG_LEN - 1u);
        bleMsg[BLE_MSG_LEN - 1u] = '\0';
        if (xQueueSend(xBleQueue, bleMsg, 0) != pdTRUE)
            RLOG("[WARN] xBleQueue full -- RESULT dropped!");
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
        RLOG("[1] YouTube track received: \"%s\" | \"%s\" | videoId=%s",
            m.track.title, m.track.artist, m.track.videoId);
        RLOG("[2] PC player notified (NORA HTTP POST)");

        if (xQueueSend(xMusicQueue, &m, 0) != pdTRUE)
            RLOG("[WARN] xMusicQueue full -- TRACK dropped");
    }
    else if (strncmp(msg, "THUMB:", 6) == 0)
    {
        s_thumbSize     = (uint32_t)atoi(msg + 6);
        s_thumbReceived = 0u;
        s_thumbMode     = true;
    }
    else if (strncmp(msg, "ERROR:", 6) == 0)
    {
        RLOG("[ERROR] NORA error: \"%s\"", msg + 6);
        music_msg_t m;
        memset(&m, 0, sizeof(m));
        m.type = MSG_ERROR;
        strncpy(m.error.reason, msg + 6, sizeof(m.error.reason) - 1u);
        if (xQueueSend(xMusicQueue, &m, 0) != pdTRUE)
            RLOG("[WARN] xMusicQueue full -- ERROR dropped");
    }
    else if (strncmp(msg, "CMD:", 4) == 0)
    {
        /* Voice command routed back from NORA (Rule B / Rule C).
         * Hand off to CommandHandler task — non-blocking; drop if queue full. */
        RLOG("[CMD] routing to CommandHandler: \"%s\"", msg);
        if (CommandHandler_Post(msg) == 0)
            RLOG("[WARN] CMD queue full -- dropped: \"%s\"", msg);
    }
    else if (strncmp(msg, "AUDIO:READY", 11) == 0)
    {
        /* NORA has opened the GCS HTTP PUT and is ready to receive WAV bytes.
         * Signal AudioSD_SendFileToUART() to start streaming. */
        AudioSD_NotifyReady();
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

    /* Enable DWT cycle counter (CM7 @ 400 MHz). */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

    LOG("\n========================================\n"
        "[UART] UARTReceiveTask started -- DMA+IDLE mode\n"
        "       UART8 |  921600 8N1 | DMA1 Stream0\n"
        "       DWT cycle counter enabled (400 MHz)\n"
        "========================================\n");

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
                RLOG("[UART] heartbeat -- idle (ISR count=%lu)", g_uartIsrCount);
            }
            continue;
        }

        TLOG("3 xRawBleQueue_recv len=%u  t=%lu us", raw.len, T_US());
        RLOG("[UART] burst: %u bytes  [%02X %02X %02X %02X]",
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
                if (b == 0x00u)        continue;  /* skip null bytes (NORA boot framing artifact) */

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
  MPU_InitStruct.BaseAddress = 0x08000000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_1MB;
  MPU_InitStruct.SubRegionDisable = 0x0;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_PRIV_RO;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_NOT_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

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
