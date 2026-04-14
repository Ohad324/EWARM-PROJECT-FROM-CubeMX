/**
 * usbd_conf.c — USB Device library HAL/LL glue for STM32H747I-DISCO
 * Uses USB1_OTG_HS with external ULPI PHY — this is CN1 on the board.
 * (USB2_OTG_FS is NOT connected to CN1 on this board — HS ULPI is.)
 * Adapted from STM32Cube_FW_H7_V1.12.1 MSC_Standalone reference (USE_USB_HS).
 */

#include "main.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include "log_mutex.h"

/* -------------------------------------------------------------------------
 * Static PCD handle
 * ---------------------------------------------------------------------- */
static PCD_HandleTypeDef s_hpcd;

/* =========================================================================
 * PCD MSP — GPIO + clock + NVIC  (USB1_OTG_HS with ULPI PHY)
 * Pin mapping from UM2411 / board schematic:
 *   PA5  = ULPI_CLK     AF10
 *   PA3  = ULPI_D0      AF10
 *   PB0  = ULPI_D1      AF10
 *   PB1  = ULPI_D2      AF10
 *   PB5  = ULPI_D3      AF10
 *   PB10 = ULPI_D4      AF10
 *   PB11 = ULPI_D5      AF10
 *   PB12 = ULPI_D6      AF10
 *   PB13 = ULPI_D7      AF10
 *   PC0  = ULPI_STP     AF10
 *   PH4  = ULPI_NXT     AF10
 *   PI11 = ULPI_DIR     AF10
 * ====================================================================== */
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
    GPIO_InitTypeDef gpio = {0};

    if (hpcd->Instance != USB_OTG_HS)
        return;

    /* USB clock (PLL3Q=48 MHz) and voltage detector are configured in
     * SystemClock_Config() / main.c — nothing to do here. */

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOI_CLK_ENABLE();

    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF10_OTG2_HS;

    /* PA5=CLK  PA3=D0 */
    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_3;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PB0=D1  PB1=D2  PB5=D3  PB10=D4  PB11=D5  PB12=D6  PB13=D7 */
    gpio.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_5 |
               GPIO_PIN_10 | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* PC0=STP */
    gpio.Pin = GPIO_PIN_0;
    HAL_GPIO_Init(GPIOC, &gpio);

    /* PH4=NXT */
    gpio.Pin = GPIO_PIN_4;
    HAL_GPIO_Init(GPIOH, &gpio);

    /* PI11=DIR */
    gpio.Pin = GPIO_PIN_11;
    HAL_GPIO_Init(GPIOI, &gpio);

    /* Enable USB HS + ULPI clocks */
    __HAL_RCC_USB_OTG_HS_CLK_ENABLE();
    __HAL_RCC_USB_OTG_HS_ULPI_CLK_ENABLE();

    HAL_NVIC_SetPriority(OTG_HS_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(OTG_HS_IRQn);
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *hpcd)
{
    if (hpcd->Instance != USB_OTG_HS)
        return;

    HAL_NVIC_DisableIRQ(OTG_HS_IRQn);
    __HAL_RCC_USB1_OTG_HS_CLK_DISABLE();
    __HAL_RCC_USB1_OTG_HS_ULPI_CLK_DISABLE();
}

/* =========================================================================
 * PCD → USB Device Library callbacks
 * ====================================================================== */
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
    { USBD_LL_SetupStage(hpcd->pData, (uint8_t *)hpcd->Setup); }

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
    { USBD_LL_DataOutStage(hpcd->pData, epnum, hpcd->OUT_ep[epnum].xfer_buff); }

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
    { USBD_LL_DataInStage(hpcd->pData, epnum, hpcd->IN_ep[epnum].xfer_buff); }

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
    { USBD_LL_SOF(hpcd->pData); }

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    USBD_SpeedTypeDef speed;
    switch (hpcd->Init.speed) {
    case PCD_SPEED_HIGH: speed = USBD_SPEED_HIGH; break;
    default:             speed = USBD_SPEED_FULL;  break;
    }
    USBD_LL_Reset(hpcd->pData);
    USBD_LL_SetSpeed(hpcd->pData, speed);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
    { USBD_LL_Suspend(hpcd->pData); }

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
    { USBD_LL_Resume(hpcd->pData); }

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
    { USBD_LL_IsoOUTIncomplete(hpcd->pData, epnum); }

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
    { USBD_LL_IsoINIncomplete(hpcd->pData, epnum); }

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
    SEGGER_RTT_WriteString(0, "[USB] CONNECT\n");   /* ISR context — RTT only */
    USBD_LL_DevConnected(hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
    SEGGER_RTT_WriteString(0, "[USB] DISCONNECT\n");   /* ISR context — RTT only */
    USBD_LL_DevDisconnected(hpcd->pData);
}

/* =========================================================================
 * PCD handle accessor — for OTG_HS_IRQHandler in stm32h7xx_it.c
 * ====================================================================== */
PCD_HandleTypeDef *USB_MSC_GetPCDHandle(void) { return &s_hpcd; }

/* =========================================================================
 * USB Device Library → PCD (LL interface)
 * ====================================================================== */
USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    s_hpcd.Instance                 = USB_OTG_HS;
    s_hpcd.Init.dev_endpoints       = 8;
    s_hpcd.Init.use_dedicated_ep1   = 0;
    s_hpcd.Init.dma_enable          = 0;
    s_hpcd.Init.low_power_enable    = 0;
    s_hpcd.Init.lpm_enable          = 0;
    s_hpcd.Init.phy_itface          = PCD_PHY_ULPI;
    s_hpcd.Init.Sof_enable          = 0;
    s_hpcd.Init.speed               = PCD_SPEED_HIGH;
    s_hpcd.Init.vbus_sensing_enable = 0;
    s_hpcd.Init.use_external_vbus   = 0;

    s_hpcd.pData = pdev;
    pdev->pData  = &s_hpcd;

    HAL_PCD_Init(&s_hpcd);

    /* FIFO sizes for HS — larger than FS */
    HAL_PCDEx_SetRxFiFo(&s_hpcd,  0x200);
    HAL_PCDEx_SetTxFiFo(&s_hpcd, 0, 0x40);
    HAL_PCDEx_SetTxFiFo(&s_hpcd, 1, 0x100);

    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
    { HAL_PCD_DeInit(pdev->pData); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
    { HAL_PCD_Start(pdev->pData); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
    { HAL_PCD_Stop(pdev->pData); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev,
                                   uint8_t ep_addr, uint8_t ep_type, uint16_t ep_mps)
    { HAL_PCD_EP_Open(pdev->pData, ep_addr, ep_mps, ep_type); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
    { HAL_PCD_EP_Close(pdev->pData, ep_addr); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
    { HAL_PCD_EP_Flush(pdev->pData, ep_addr); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
    { HAL_PCD_EP_SetStall(pdev->pData, ep_addr); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
    { HAL_PCD_EP_ClrStall(pdev->pData, ep_addr); return USBD_OK; }

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *h = pdev->pData;
    if ((ep_addr & 0x80u) == 0x80u)
        return h->IN_ep[ep_addr & 0x7Fu].is_stall;
    else
        return h->OUT_ep[ep_addr & 0x7Fu].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
    { HAL_PCD_SetAddress(pdev->pData, dev_addr); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev,
                                    uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
    { HAL_PCD_EP_Transmit(pdev->pData, ep_addr, pbuf, size); return USBD_OK; }

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev,
                                          uint8_t ep_addr, uint8_t *pbuf, uint32_t size)
    { HAL_PCD_EP_Receive(pdev->pData, ep_addr, pbuf, size); return USBD_OK; }

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
    { return HAL_PCD_EP_GetRxCount(pdev->pData, ep_addr); }

/* Static allocator — sized for USBD_MSC_BOT_HandleTypeDef (includes 8KB bot_data).
 * Placed in D2 SRAM1 (.usb_msc_bss) — same region as s_mscSectorBuf.
 * This ensures bot_data (USB DMA target) is in the same cacheable region
 * where we do SCB_CleanDCache / SCB_InvalidateDCache around SD transfers. */
#pragma data_alignment = 32
#pragma location = ".usb_msc_bss"
static uint32_t s_usbMem[(sizeof(USBD_MSC_BOT_HandleTypeDef) / 4u) + 1u];

void *USBD_static_malloc(uint32_t size)
{
    (void)size;
    uint32_t *mem = s_usbMem;
    return mem;
}

void USBD_static_free(void *p) { (void)p; }

void USBD_LL_Delay(uint32_t delay) { HAL_Delay(delay); }
