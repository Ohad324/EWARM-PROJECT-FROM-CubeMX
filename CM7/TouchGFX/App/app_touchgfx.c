/**
  ******************************************************************************
  * File Name          : app_touchgfx.c
  ******************************************************************************
  * This file was created by TouchGFX Generator 4.26.1. This file is only
  * generated once! Delete this file from your project and re-generate code
  * using STM32CubeMX or change this file manually to update it.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

/* Includes ------------------------------------------------------------------*/
#include "app_touchgfx.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

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
void touchgfx_init(void);
void touchgfx_components_init(void);
void touchgfx_taskEntry(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/**
 * PreOS Initialization function
 */
void MX_TouchGFX_PreOSInit(void)
{
}

/**
 * Initialize TouchGFX application
 */
void MX_TouchGFX_Init(void)
{
    // Calling forward to touchgfx_init in C++ domain
    touchgfx_components_init();
    touchgfx_init();
}

/**
 * TouchGFX application entry function
 */
void MX_TouchGFX_Process(void)
{
    // Calling forward to touchgfx_taskEntry in C++ domain
    touchgfx_taskEntry();
}

/* GATED MULTITASKING — declarations for the deferred init calls. These are
 * static in main.c, so we declare them here as extern. */
extern void MX_DSIHOST_DSI_Init(void);
extern void MX_LTDC_Init(void);

/**
 * TouchGFX application thread
 *
 * GATED MULTITASKING (CLAUDE.md, 2026-05-04): Phase 2 hardware activation.
 * DSI host, LTDC, and TouchGFX framework are initialized HERE, not in
 * main(), so that:
 *   1. Phase 1 (in main(), before osKernelStart()) runs CPU-heavy work
 *      while LTDC is physically Disabled — no FUIF possible.
 *   2. Phase 2 (this task) starts the display only after the framebuffer
 *      is already perfect in SDRAM.
 *   3. First HAL_DSI_Refresh() lands a clean frame on the panel GRAM.
 */
void TouchGFX_Task(void* argument)
{
    /* Phase 2: Hardware Activation — display peripherals come up here */
    MX_DSIHOST_DSI_Init();
    MX_LTDC_Init();
    MX_TouchGFX_Init();

    /* Enter the framework's main loop (never returns) */
    touchgfx_taskEntry();
}

/************************ (C) COPYRIGHT STMicroelectronics *****END OF FILE****/
