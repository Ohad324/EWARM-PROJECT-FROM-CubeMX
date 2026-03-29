/**
 * @file    ble_queue.h
 * @brief   Shared FreeRTOS queue for BLE UART messages.
 *          Included by both main.c (C) and Model.cpp (C++).
 */
#ifndef BLE_QUEUE_H
#define BLE_QUEUE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "FreeRTOS.h"
#include "queue.h"

/** Maximum BLE message length including null terminator */
#define BLE_MSG_LEN  64

/** Queue handle — defined in main.c, used in Model.cpp */
extern QueueHandle_t xBleQueue;

#ifdef __cplusplus
}
#endif

#endif /* BLE_QUEUE_H */
