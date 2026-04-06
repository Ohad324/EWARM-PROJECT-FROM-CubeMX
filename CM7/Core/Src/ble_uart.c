/**
 * @file    ble_uart.c
 * @brief   DMA + IDLE-line receive driver for UART8 (BLE module).
 *
 * Why DMA + IDLE instead of polling or byte-at-a-time interrupt?
 *
 *   Polling (old code): HAL_UART_Receive() blocks the task for up to
 *   `timeout` ms per byte.  When TouchGFX preempts UARTReceiveTask, the
 *   UART peripheral FIFO (4 bytes on STM32H7) overflows and bytes are
 *   permanently lost before the task wakes.
 *
 *   DMA + IDLE: the DMA engine copies bytes from the UART data register
 *   directly into s_dma_rx_buf[] without CPU involvement.  The IDLE line
 *   interrupt fires ~1 bit-time after the last byte of a burst, delivering
 *   a complete, byte-perfect snapshot of the BLE message.  TouchGFX can
 *   run at full speed; the DMA fills the buffer in parallel.
 *
 * D-Cache coherency on STM32H7:
 *   The Cortex-M7 D-Cache is enabled in this project (SCB_EnableDCache()).
 *   DMA writes go to physical RAM, bypassing the cache.  Before the CPU
 *   reads s_dma_rx_buf, we call SCB_InvalidateDCache_by_Addr() to discard
 *   the stale cached values and force a fresh fetch from RAM.
 *   The buffer is 32-byte aligned so the invalidation covers exactly
 *   BLE_DMA_BUF_SIZE bytes without touching adjacent variables.
 *
 * Memory placement:
 *   s_dma_rx_buf must be in a DMA-accessible SRAM region.
 *   On STM32H747, AXI SRAM (0x24000000) and SRAM1-3 (0x30000000) are
 *   accessible by DMA1.  DTCM (0x20000000) is CPU-only — never place the
 *   DMA buffer there.  The default .bss placement in IAR's linker script
 *   for this board puts globals in AXI SRAM, which is correct.
 *   If you use a custom .icf that places .bss in DTCM, add a section:
 *     place in AXI_RAM { section .dma_buf };
 *   and annotate the variable:
 *     __attribute__((section(".dma_buf")))
 */

#include "ble_uart.h"
#include "ble_queue.h"       /* xBleQueue, BLE_MSG_LEN — consumed by Model::tick() */
#include "main.h"            /* Error_Handler(), huart8 */
#include "stm32h7xx_hal.h"
#include "SEGGER_RTT.h"
#include <string.h>
#include <stdio.h>

/* RTT_LOG — uses only SEGGER_RTT_Write (no SEGGER_RTT_printf.c required) */
#define RTT_LOG(fmt, ...) do { \
    char _b[128]; \
    int  _l = snprintf(_b, sizeof(_b), fmt, ##__VA_ARGS__); \
    if (_l > 0) SEGGER_RTT_Write(0, _b, (unsigned)_l); \
} while (0)

/* ── External handle (defined in main.c) ───────────────────────────────────── */
extern UART_HandleTypeDef huart8;

/* ── Private: DMA handle ────────────────────────────────────────────────────── */

/**
 * DMA handle for UART8 RX.
 *
 * Kept static (private to this file) so the rest of the project cannot
 * accidentally call HAL_DMA_Start() on it.  stm32h7xx_it.c routes the
 * DMA1_Stream0 vector through BLE_UART_DMA_IRQHandler() instead.
 *
 * Stream assignment: DMA1 Stream0, DMAMUX1 request UART8_RX (ID 43).
 * If Stream0 is already used by another peripheral, change Instance to
 * DMA1_Stream1 … DMA1_Stream7 or any free DMA2 stream — the DMAMUX
 * request stays the same regardless of stream.
 */
static DMA_HandleTypeDef s_hdma_uart8_rx;

/* ── Private: DMA receive buffer ────────────────────────────────────────────── */

/**
 * Raw byte buffer written by DMA, read in HAL_UARTEx_RxEventCallback.
 *
 * Alignment constraint: SCB_InvalidateDCache_by_Addr() requires the start
 * address to be 32-byte aligned.  __attribute__((aligned(32))) satisfies this.
 *
 * Size constraint: must be a multiple of 32 so the cache invalidation does
 * not extend past the end of the array into adjacent variables.
 * 128 = 4 × 32, handles messages up to 127 printable bytes.
 */
static uint8_t s_dma_rx_buf[BLE_DMA_BUF_SIZE] __attribute__((aligned(32), section(".dma_buf")));

/* ── DMA memory region validation ───────────────────────────────────────────── */

void validate_dma_buffers(void)
{
    uint32_t addr = (uint32_t)s_dma_rx_buf;
    if (addr >= 0x20000000UL && addr <= 0x2001FFFFUL)
    {
        RTT_LOG(
            "[DMA] FATAL: s_dma_rx_buf @ 0x%08lX is in DTCM — "
            "DMA1 cannot access this! Fix linker script.\n", addr);
    }
    else
    {
        RTT_LOG(
            "[DMA] s_dma_rx_buf @ 0x%08lX — OK (DMA accessible)\n", addr);
    }
}

/* ── Public: shared RTOS objects ────────────────────────────────────────────── */

char              bleHistory[BLE_HIST_ROWS][BLE_HIST_COLS];
SemaphoreHandle_t xBleHistMutex;
QueueHandle_t     xRawBleQueue;

/* ── Private helper ─────────────────────────────────────────────────────────── */

/**
 * History_Push — internal implementation called by BLE_UART_HistPush().
 *
 * Why memmove and not a loop?
 *   memmove is optimised to handle overlapping regions correctly and is
 *   typically implemented with SIMD/memcpy internals — faster than a byte
 *   loop for 9 × 32 = 288 bytes.
 */
static void History_Push(const char *msg)
{
    if (xSemaphoreTake(xBleHistMutex, pdMS_TO_TICKS(5)) == pdTRUE)
    {
        /* Shift rows 0..8 → 1..9, discarding the oldest message (was at 9).
         * Array pointers carry row-count information so the analyser can
         * verify the full (BLE_HIST_ROWS-1)*BLE_HIST_COLS extent. */
        char (*src)[BLE_HIST_COLS] = &bleHistory[0];
        char (*dst)[BLE_HIST_COLS] = &bleHistory[1];
        memmove(dst, src, (BLE_HIST_ROWS - 1u) * sizeof(*src));

        /* Copy new message into row 0; guarantee null-termination. */
        (void)strncpy(bleHistory[0], msg, BLE_HIST_COLS - 1u);
        bleHistory[0][BLE_HIST_COLS - 1u] = '\0';

        xSemaphoreGive(xBleHistMutex);
    }
    /* If the mutex take times out (should never happen in normal operation),
       we silently drop the history update.  The message still reaches
       Model::tick() via xBleQueue. */
}

/* ── Public API ─────────────────────────────────────────────────────────────── */

/**
 * BLE_UART_Init — one-time hardware and RTOS setup.
 *
 * Call order in main():
 *   MX_UART8_Init();   ← configures UART peripheral and GPIO
 *   BLE_UART_Init();   ← this function: attaches DMA, creates RTOS objects
 *   osKernelStart();
 *
 * Why not put DMA setup in HAL_UART_MspInit?
 *   HAL_UART_MspInit is generated by CubeMX and gets overwritten on
 *   regeneration.  Keeping DMA setup here avoids that problem.
 */
void BLE_UART_Init(void)
{
    /* ── 1. Enable DMA1 clock ──────────────────────────────────────────── */
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* ── 2. Configure DMA handle ───────────────────────────────────────── */
    s_hdma_uart8_rx.Instance                 = DMA1_Stream0;

    /* DMAMUX1 request 43: UART8_RX.  The DMAMUX routes any peripheral
       request to any DMA stream; the peripheral register address is set
       automatically by HAL_UART_Receive_DMA internally. */
    s_hdma_uart8_rx.Init.Request             = DMA_REQUEST_UART8_RX;
    s_hdma_uart8_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_hdma_uart8_rx.Init.PeriphInc           = DMA_PINC_DISABLE;  /* UART DR is fixed */
    s_hdma_uart8_rx.Init.MemInc              = DMA_MINC_ENABLE;   /* advance through buf */
    s_hdma_uart8_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_hdma_uart8_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;

    /* Normal (not Circular) mode: DMA stops after BLE_DMA_BUF_SIZE bytes.
       Why normal and not circular?
         Circular mode re-wraps the write pointer at the buffer end, so a
         long message would silently overwrite its own beginning.  Normal mode
         fires Transfer-Complete when full, giving us a clean boundary.
         We restart manually in the callback for the next message. */
    s_hdma_uart8_rx.Init.Mode               = DMA_NORMAL;
    s_hdma_uart8_rx.Init.Priority           = DMA_PRIORITY_HIGH;
    s_hdma_uart8_rx.Init.FIFOMode           = DMA_FIFOMODE_DISABLE;

    if (HAL_DMA_Init(&s_hdma_uart8_rx) != HAL_OK)
    {
        Error_Handler();
    }

    /* Link the DMA handle to huart8 so HAL_UARTEx_ReceiveToIdle_DMA()
       knows which stream to start. */
    __HAL_LINKDMA(&huart8, hdmarx, s_hdma_uart8_rx);
    RTT_LOG( "[UART] hdmarx linked: %p (NULL=bad)\n", (void*)huart8.hdmarx);
    validate_dma_buffers();

    /* ── 3. DMA stream IRQ ─────────────────────────────────────────────── */
    /* Priority rule: must be numerically >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY
       (= 5 in this project) so that xQueueSendFromISR() is legal inside the
       callback.  Priority 6 is one notch below the UART8 IRQ (5) so the UART
       IDLE handler can pre-empt a DMA TC handler if both fire simultaneously. */
    HAL_NVIC_SetPriority(DMA1_Stream0_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(DMA1_Stream0_IRQn);
    /* Note: UART8_IRQn (needed for IDLE line detection) is already enabled at
       priority 5 by HAL_UART_MspInit() called from MX_UART8_Init().
       Do not re-configure it here. */

    /* ── 4. FreeRTOS objects ───────────────────────────────────────────── */

    /* Internal ISR→task queue.  4 slots of BleRawMsg_t (4 × 129 = 516 bytes
       on the heap).  4 slots absorbs a rapid burst of back-to-back BLE
       packets without the ISR having to drop any, even if UARTReceiveTask
       is briefly pre-empted by a TouchGFX frame. */
    xRawBleQueue = xQueueCreate(4u, sizeof(BleRawMsg_t));
    configASSERT(xRawBleQueue != NULL);

    /* Mutex for bleHistory read/write synchronisation. */
    xBleHistMutex = xSemaphoreCreateMutex();
    configASSERT(xBleHistMutex != NULL);

    /* Zero the history so the TouchGFX task never reads uninitialised data
       on the first frame before any BLE message has arrived. */
    memset(bleHistory, 0, sizeof(bleHistory));
}

/**
 * BLE_UART_StartDMA — arm the first receive-to-idle transfer.
 *
 * Called once from UARTReceiveTask after osKernelStart().  Must run in
 * task context (not from main() before the scheduler) because:
 *   • The FreeRTOS ISR infrastructure (portNVIC_*) must be live.
 *   • Any early IDLE interrupt before xRawBleQueue exists would fault.
 */
void BLE_UART_StartDMA(void)
{
    RTT_LOG( "[UART] huart8.hdmarx: %p\n", (void*)huart8.hdmarx);

    /* Verify buffer is in DMA-accessible memory before arming */
    uint32_t buf_addr = (uint32_t)s_dma_rx_buf;
    if (buf_addr >= 0x20000000UL && buf_addr <= 0x2001FFFFUL)
    {
        RTT_LOG(
            "[UART] FATAL: DMA buffer in DTCM 0x%08lX — DMA1 cannot access!\n",
            buf_addr);
    }
    else
    {
        RTT_LOG( "[UART] DMA buffer OK: 0x%08lX\n", buf_addr);
    }

    /* NORA may send data before DMA is armed, causing ORE (Overrun Error).
     * Clear hardware error flags, software ErrorCode, and abort the DMA handle
     * so HAL_DMA_Start_IT finds the DMA in READY state. */
    __HAL_UART_CLEAR_FLAG(&huart8, UART_CLEAR_OREF | UART_CLEAR_FEF |
                                    UART_CLEAR_NEF  | UART_CLEAR_IDLEF);
    huart8.ErrorCode = HAL_UART_ERROR_NONE;
    huart8.RxState   = HAL_UART_STATE_READY;
    if (huart8.hdmarx != NULL)
    {
        HAL_DMA_Abort(huart8.hdmarx);   /* resets DMA handle State to READY */
    }

    HAL_StatusTypeDef ret = HAL_UARTEx_ReceiveToIdle_DMA(&huart8,
                                                          s_dma_rx_buf,
                                                          BLE_DMA_BUF_SIZE);
    if (ret != HAL_OK)
    {
        RTT_LOG(
            "[UART] ERROR: DMA arm failed! ret=%d gState=0x%08lX RxState=0x%08lX ErrorCode=0x%08lX\n",
            (int)ret, (uint32_t)huart8.gState, (uint32_t)huart8.RxState, huart8.ErrorCode);
        /* Do NOT call Error_Handler — system keeps running, LCD still works */
        return;
    }
    RTT_LOG( "[UART] DMA armed OK\n");

    /* Disable half-transfer interrupt — we only want TC and IDLE. */
    __HAL_DMA_DISABLE_IT(&s_hdma_uart8_rx, DMA_IT_HT);
}

/**
 * BLE_UART_HistPush — public wrapper around History_Push.
 *
 * Exposed so UARTReceiveTask (in main.c) can update the history without
 * needing access to the static s_hdma_uart8_rx handle or internal helpers.
 */
void BLE_UART_HistPush(const char *msg)
{
    History_Push(msg);
}

/**
 * BLE_UART_DMA_IRQHandler — forwards DMA1_Stream0 interrupts to the HAL.
 *
 * The DMA handle is private (static) to this file.  stm32h7xx_it.c calls
 * this function from DMA1_Stream0_IRQHandler so it never needs to know the
 * handle.  HAL_DMA_IRQHandler processes TC, HT, and TE flags internally and
 * triggers HAL_UARTEx_RxEventCallback when a transfer event completes.
 */
void BLE_UART_DMA_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_hdma_uart8_rx);
}

/* ── HAL callback override (ISR context) ─────────────────────────────────────
 *
 * HAL_UARTEx_RxEventCallback is declared __weak in the HAL library.
 * Defining it here replaces the default empty implementation.
 *
 * This function is called from:
 *   • DMA1_Stream0_IRQHandler  → when DMA transfer is complete (buffer full)
 *   • UART8_IRQHandler         → when IDLE line is detected (end of burst)
 *
 * Execution is at IRQ priority 5 or 6 — within the FreeRTOS syscall range,
 * so all FromISR API calls are legal.
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * HAL_UARTEx_RxEventCallback — ISR handler for every completed DMA receive.
 *
 * @param huart  UART handle that triggered the event.
 * @param Size   Number of bytes actually received into s_dma_rx_buf[].
 *
 * Steps:
 *  1. Guard against callbacks from other UART peripherals.
 *  2. Invalidate D-Cache so the CPU reads DMA-written data, not stale cache.
 *  3. Copy the received bytes into a BleRawMsg_t (so we can safely restart
 *     DMA in step 4 without racing the next message into the same buffer).
 *  4. Post the raw message to xRawBleQueue (non-blocking; drop if full).
 *  5. Restart DMA for the next incoming message.
 *  6. Yield to UARTReceiveTask if the queue post raised its priority.
 */
extern volatile uint32_t g_uartIsrCount;

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    /* Step 1: ignore events from any UART other than our BLE UART. */
    if (huart->Instance != UART8)
    {
        return;
    }
    g_uartIsrCount++;

    if (Size == 0)
    {
        RTT_LOG(
            "[UART] WARNING: RxEventCallback Size=0 — "
            "possible DMA memory access error!\n");
        return;
    }

    /* Step 2: D-Cache invalidation.
       The DMA engine wrote to physical RAM.  The Cortex-M7 data cache still
       holds the old content from before DMA started.  Invalidating these cache
       lines forces the CPU to fetch fresh data from RAM on the next read.
       The 32-byte alignment of s_dma_rx_buf guarantees no adjacent variable
       is evicted by this call. */
    SCB_InvalidateDCache_by_Addr((uint32_t *)s_dma_rx_buf, BLE_DMA_BUF_SIZE);

    /* Step 3: Copy into a local struct.
       Clamp Size to BLE_DMA_BUF_SIZE in the (unlikely) event the HAL reports
       a count larger than the buffer. */
    BleRawMsg_t raw;
    raw.len = (Size <= BLE_DMA_BUF_SIZE) ? Size : BLE_DMA_BUF_SIZE;
    memcpy(raw.data, s_dma_rx_buf, raw.len);

    /* Step 4: Post to the internal queue.
       xHigherPriorityTaskWoken is set to pdTRUE if posting unblocks
       UARTReceiveTask and it has a higher priority than the interrupted task. */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    (void)xQueueSendFromISR(xRawBleQueue, &raw, &xHigherPriorityTaskWoken);
    /* If the queue is full we drop this message.  At 115200 baud a message
       takes > 0.5 ms; dropping is extremely unlikely in normal operation. */

    /* Step 5: Restart DMA immediately so the next BLE transmission is
       captured without any gap.  By the time UARTReceiveTask wakes to
       process raw, DMA is already listening again.
       If the UART has a pending overrun/framing error (ORE/FE — can happen
       when the ISR runs slowly and the FIFO fills), clear the error flags
       and reset the HAL state so ReceiveToIdle_DMA succeeds on the retry. */
    if (HAL_UARTEx_ReceiveToIdle_DMA(&huart8, s_dma_rx_buf, BLE_DMA_BUF_SIZE) != HAL_OK)
    {
        __HAL_UART_CLEAR_FLAG(&huart8, UART_CLEAR_OREF  |
                                        UART_CLEAR_FEF   |
                                        UART_CLEAR_NEF   |
                                        UART_CLEAR_IDLEF);
        huart8.RxState = HAL_UART_STATE_READY;
        (void)HAL_UARTEx_ReceiveToIdle_DMA(&huart8, s_dma_rx_buf, BLE_DMA_BUF_SIZE);
    }
    __HAL_DMA_DISABLE_IT(&s_hdma_uart8_rx, DMA_IT_HT);

    /* Step 6: If UARTReceiveTask was unblocked and outranks the currently
       running task, request an immediate context switch so it runs the
       moment the ISR returns — minimising message latency. */
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

/**
 * HAL_DMA_ErrorCallback — called by HAL when a DMA transfer error occurs.
 * Overrides the __weak default.  Logs the error immediately via RTT.
 */
void HAL_DMA_ErrorCallback(DMA_HandleTypeDef *hdma)
{
    RTT_LOG(
        "[DMA] ERROR: DMA transfer error! ErrorCode=0x%08lX\n",
        hdma->ErrorCode);
}

/**
 * HAL_UART_ErrorCallback — called by HAL on UART framing/overrun/noise errors.
 * Overrides the __weak default.
 *
 * When NORA reboots its UART TX line glitches (framing error on UART8).
 * Without this callback, HAL leaves UART8 in error state and DMA stops.
 * Here we clear the error flags and restart DMA so reception resumes.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != UART8) return;

    RTT_LOG("[UART] ErrorCallback: err=0x%08lX — clearing + re-arming DMA\n",
            (unsigned long)huart->ErrorCode);

    __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF  |
                                  UART_CLEAR_FEF   |
                                  UART_CLEAR_NEF   |
                                  UART_CLEAR_IDLEF);
    huart->ErrorCode = HAL_UART_ERROR_NONE;
    huart->RxState   = HAL_UART_STATE_READY;

    if (HAL_UARTEx_ReceiveToIdle_DMA(huart, s_dma_rx_buf, BLE_DMA_BUF_SIZE) != HAL_OK)
    {
        RTT_LOG("[UART] ErrorCallback: re-arm failed\n");
    }
    __HAL_DMA_DISABLE_IT(&s_hdma_uart8_rx, DMA_IT_HT);
}
