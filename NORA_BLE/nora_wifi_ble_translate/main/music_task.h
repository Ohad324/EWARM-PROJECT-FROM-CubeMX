#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Initialize the music task.
 * Call once from app_main() after UART mutex is created.
 * @param uart_mutex  Existing UART mutex shared with translation task
 */
void music_task_init(SemaphoreHandle_t uart_mutex);

/**
 * Enqueue a YouTube search + thumbnail request.
 * Call from BLE write handler when "PLAY:" prefix is detected.
 * @param query  UTF-8 search query (after "PLAY:" prefix stripped)
 */
void music_request(const char *query);
