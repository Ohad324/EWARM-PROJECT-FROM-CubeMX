/**
 * @file  music_display_task.h
 * @brief FreeRTOS task and queues for Music Display (Phase 1).
 *
 * Data flow:
 *   UARTReceiveTask  -->  xMusicQueue     (raw  music_msg_t)
 *   Music_Poll()     <--  xMusicQueue     (called from Model::tick())
 *   Music_Poll()     -->  xMusicDoneQueue (done music_done_msg_t)
 *   Model::tick()    <--  xMusicDoneQueue
 */
#ifndef MUSIC_DISPLAY_TASK_H
#define MUSIC_DISPLAY_TASK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "FreeRTOS.h"
#include "queue.h"

/* ── Raw message: UARTReceiveTask → JpegDisplayTask ──────────────────── */

typedef enum {
    MSG_TRACK = 0,
    MSG_THUMB = 1,
    MSG_ERROR = 2
} music_msg_type_t;

typedef struct {
    music_msg_type_t type;
    union {
        struct {
            char title[128];
            char artist[64];
            char videoId[32];
        } track;
        struct {
            uint8_t  *data;   /* pointer to s_jpegInBuf in main.c */
            uint32_t  size;
        } thumb;
        struct {
            char reason[64];
        } error;
    };
} music_msg_t;

/* ── Processed result: JpegDisplayTask → Model::tick() ─────────────── */

typedef enum {
    MUSIC_DONE_TRACK = 0,
    MUSIC_DONE_THUMB = 1,
    MUSIC_DONE_ERROR = 2
} music_done_type_t;

typedef struct {
    music_done_type_t type;
    union {
        struct {
            char title[128];
            char artist[64];
            char videoId[32];
        } track;
        struct {
            uint8_t  *rgb888;   /* 800x480 scaled output, valid until next THUMB */
            uint32_t  width;    /* decoded source width  (e.g. 1280) */
            uint32_t  height;   /* decoded source height (e.g.  720) */
        } thumb;
        struct {
            char reason[64];
        } error;
    };
} music_done_msg_t;

/* ── Queue handles (defined in music_display_task.c) ─────────────────── */
extern QueueHandle_t xMusicQueue;
extern QueueHandle_t xMusicDoneQueue;

/* ── Public API ──────────────────────────────────────────────────────── */

/** Create xMusicQueue and xMusicDoneQueue.
 *  Call from main() after osKernelInitialize(), before osKernelStart(). */
void Music_Init(void);

/** Non-blocking poll — call from Model::tick() every TouchGFX frame.
 *  Checks xMusicQueue (timeout=0); if a message is waiting, processes it:
 *    MSG_TRACK/MSG_ERROR  → forwarded to xMusicDoneQueue immediately.
 *    MSG_THUMB            → JPEG_Decode() called (blocks ~200–500 ms),
 *                           result posted to xMusicDoneQueue.
 *  Returns immediately when the queue is empty. */
void Music_Poll(void);

/** Phase 2 stub: send CTRL:<action>\n over UART8 TX to NORA.
 *  Defined but NOT called in Phase 1. Uncomment transmit in Phase 2. */
void Music_SendCtrl(const char *action);

#ifdef __cplusplus
}
#endif

#endif /* MUSIC_DISPLAY_TASK_H */
