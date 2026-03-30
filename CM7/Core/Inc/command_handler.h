/*
 * command_handler.h — STM32-side voice command receiver
 *
 * Receives CMD:<text>\n messages from NORA over UART8 and dispatches
 * them to the appropriate screen-update function.
 *
 * MESSAGE FORMAT (from NORA):
 *   "CMD:show temperature\n"  → display temperature on screen
 *   "CMD:clear screen\n"      → clear the display
 *   "CMD:UNKNOWN\n"           → log unrecognised command
 *
 * INTEGRATION:
 *   1. Call CommandHandler_Init() from main() before osKernelStart().
 *   2. Call xTaskCreate(CommandHandler_TaskEntry, ...) to create the task.
 *   3. From UARTReceiveTask, when a message starts with "CMD:", call
 *      CommandHandler_Post(msg) to hand it off.
 */

#ifndef COMMAND_HANDLER_H
#define COMMAND_HANDLER_H

#include <stdint.h>

/* Maximum length of a CMD: payload (excluding the "CMD:" prefix and newline). */
#define CMD_MAX_LEN   60u

/* ── Public API ─────────────────────────────────────────────────────────────── */

/*
 * CommandHandler_Init — create the internal queue.
 * Call once from main() before osKernelStart().
 */
void CommandHandler_Init(void);

/*
 * CommandHandler_TaskEntry — FreeRTOS task; blocks on the command queue,
 * parses and dispatches commands to the TouchGFX/screen layer.
 * Stack recommendation: 1024 words.  Priority: below normal.
 */
void CommandHandler_TaskEntry(void *arg);

/*
 * CommandHandler_Post — copy msg into the command queue.
 * msg must be null-terminated; the "CMD:" prefix is stripped internally.
 * Safe to call from any task (not from ISR — use FromISR variant if needed).
 * Returns pdTRUE if the message was queued, pdFALSE if the queue is full.
 */
int  CommandHandler_Post(const char *msg);

#endif /* COMMAND_HANDLER_H */
