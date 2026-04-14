/**
 * @file    rtt_log_task.h
 * @brief   RTTLogTask — deferred log queue drain + SD-DETECT LED mirror.
 *
 * RTTLogTask is the sole consumer of xLogQueue.
 * It runs at priority 1 (lowest application priority) and is the only
 * task that calls printf (Terminal I/O) and SEGGER_RTT_Write simultaneously.
 * printf is safe here: any task with priority >= 2 preempts the ITM spin-wait.
 */

#ifndef RTT_LOG_TASK_H
#define RTT_LOG_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

/* FreeRTOS task entry — pass to xTaskCreate, arg = NULL */
void RTTLogTask(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* RTT_LOG_TASK_H */
