/**
 * @file    ble_uart.h
 * @brief   DMA + IDLE-line receive driver for UART8 (BLE module).
 *
 * Problem solved: polling HAL_UART_Receive() inside UARTReceiveTask is
 * preempted by TouchGFX rendering, leaving the UART FIFO unserviced.
 * DMA transfers bytes autonomously into RAM while the CPU renders; an IDLE
 * line interrupt fires at the end of every transmission burst, giving an
 * atomic, complete snapshot of the message with no bytes lost.
 *
 * Data flow:
 *   BLE → UART8 pin → DMA1 Stream0 → s_dma_rx_buf[]
 *                      (IDLE fires)
 *                      HAL_UARTEx_RxEventCallback()  [ISR]
 *                        invalidate D-Cache, copy → BleRawMsg_t
 *                        xQueueSendFromISR(xRawBleQueue)
 *                        restart DMA
 *                      UARTReceiveTask()             [task]
 *                        strip CR/LF, null-terminate
 *                        BLE_UART_HistPush()  → bleHistory[][]
 *                        xQueueSend(xBleQueue) → Model::tick()
 *
 * Add to IAR project:
 *   CM7/Core/Src/ble_uart.c
 *   CM7/Core/Inc/ble_uart.h  (already included via main.h or directly)
 */

#ifndef BLE_UART_H
#define BLE_UART_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "FreeRTOS.h"
#include "semphr.h"
#include "queue.h"
#include "ble_queue.h"   /* BLE_MSG_LEN — single authoritative definition */

/* ── Sizing constants ───────────────────────────────────────────────────────── */

/**
 * DMA receive buffer length in bytes.
 * Rules:
 *   • Must be a multiple of 32 — the Cortex-M7 D-Cache line size.
 *     SCB_InvalidateDCache_by_Addr() operates on whole cache lines; a
 *     non-multiple size would corrupt adjacent data.
 *   • Must be ≥ BLE_MSG_LEN (the longest possible BLE message).
 */
#define BLE_DMA_BUF_SIZE   128u

/* BLE_MSG_LEN is defined in ble_queue.h — do not redefine here. */

/** Number of history slots (rows). */
#define BLE_HIST_ROWS      10u

/**
 * Characters per history slot including null terminator.
 * Messages longer than 31 printable characters are truncated.
 */
#define BLE_HIST_COLS      32u

/* ── Raw-message type (ISR → UARTReceiveTask) ───────────────────────────────── */

/**
 * BleRawMsg_t — carries a raw DMA snapshot from ISR to task.
 *
 * Why copy instead of passing a pointer to s_dma_rx_buf?
 *   Because DMA is restarted immediately after the callback, reusing
 *   s_dma_rx_buf for the next incoming message.  The copy is small
 *   (≤ 128 bytes) and happens at ISR speed, long before the task wakes.
 */
typedef struct {
    uint8_t  data[BLE_DMA_BUF_SIZE];
    uint16_t len;
} BleRawMsg_t;

/* ── Shared objects (defined in ble_uart.c) ────────────────────────────────── */

/**
 * Rolling message log — index 0 is the most recent message.
 *
 * Write path: UARTReceiveTask → BLE_UART_HistPush() under xBleHistMutex.
 * Read  path: any task (TouchGFX, debug) after taking xBleHistMutex.
 *
 * Always null-terminated; initialised to all-zeros by BLE_UART_Init().
 */
extern char              bleHistory[BLE_HIST_ROWS][BLE_HIST_COLS];

/**
 * Mutex protecting bleHistory.
 * Take before reading or writing; hold as briefly as possible.
 */
extern SemaphoreHandle_t xBleHistMutex;

/**
 * Internal queue between HAL_UARTEx_RxEventCallback() [ISR] and
 * UARTReceiveTask [task].  4 BleRawMsg_t slots; posted with
 * xQueueSendFromISR(), consumed with xQueueReceive().
 */
extern QueueHandle_t     xRawBleQueue;

/* ── Public API ─────────────────────────────────────────────────────────────── */

/**
 * BLE_UART_Init — configure DMA1 Stream0 for UART8 RX and create RTOS objects.
 *
 * Call after MX_UART8_Init() (UART handle must be ready) and before
 * osKernelStart() (RTOS objects are created here, not yet running).
 *
 * DMA stream selection: DMA1 Stream0 via DMAMUX1 request UART8_RX.
 * Verify in your CubeMX DMA view that Stream0 is free; reassign the
 * Instance field if another peripheral already uses it.
 */
void BLE_UART_Init(void);

/**
 * BLE_UART_StartDMA — arm the first HAL_UARTEx_ReceiveToIdle_DMA() transfer.
 *
 * Call once from UARTReceiveTask (task context) after the scheduler starts.
 * Must NOT be called before osKernelStart(): the HAL enables UART/DMA IRQs
 * internally and the FreeRTOS ISR handlers must be ready first.
 * After the first message, the ISR callback restarts DMA automatically.
 */
void BLE_UART_StartDMA(void);

/**
 * BLE_UART_HistPush — insert a new message at index 0 of bleHistory.
 *
 * Shifts existing rows 0..8 down to 1..9 (row 9 is discarded).
 * Truncates msg to BLE_HIST_COLS − 1 characters and null-terminates.
 * Protected by xBleHistMutex; safe to call from task context only.
 */
void BLE_UART_HistPush(const char *msg);

/**
 * BLE_UART_DMA_IRQHandler — trampoline for DMA1_Stream0_IRQHandler.
 *
 * Keeps the DMA handle private to ble_uart.c while letting
 * stm32h7xx_it.c route the vector without knowing the handle.
 *
 * Add to stm32h7xx_it.c:
 *   extern void BLE_UART_DMA_IRQHandler(void);
 *   void DMA1_Stream0_IRQHandler(void) { BLE_UART_DMA_IRQHandler(); }
 */
void BLE_UART_DMA_IRQHandler(void);

/**
 * validate_dma_buffers — asserts all DMA buffers are in DMA-accessible memory.
 * Prints FATAL via SEGGER RTT if any buffer is in DTCM/ITCM.
 */
void validate_dma_buffers(void);

#ifdef __cplusplus
}
#endif

#endif /* BLE_UART_H */
