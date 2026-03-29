/*********************************************************************
*                    SEGGER RTT — minimal configuration
*                    for STM32H747 music-player debug dump
*********************************************************************/
#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H

/* Number of up/down channels allocated in the control block.
 * Channel 0: application LOG() output (Terminal 0 in RTT Viewer).
 * Channel 1: RTOS event trace — RtosTrace_DrainTask() (Terminal 1). */
#define SEGGER_RTT_MAX_NUM_UP_BUFFERS     2
#define SEGGER_RTT_MAX_NUM_DOWN_BUFFERS   1

/* Up-buffer size (target → host).
 * J-Link polls every ~1 ms.  At 4 MB/s SWD the host drains ~4 KB/ms.
 * 4 KB is the standard default and is plenty to sustain continuous flow
 * while we stream the 1.1 MB pixel dump in blocking mode.              */
#define SEGGER_RTT_BUFFER_SIZE_UP         (16384u)
#define SEGGER_RTT_BUFFER_SIZE_DOWN       (16u)

/* Default mode for channel 0.
 * SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL ensures the complete pixel buffer
 * is always transferred (at the cost of stalling MusicDisplayTask for
 * ~0.3 s while J-Link drains the pipe). Switch to
 * SEGGER_RTT_MODE_NO_BLOCK_SKIP if you never want the task to stall.  */
#define SEGGER_RTT_MODE_DEFAULT           SEGGER_RTT_MODE_NO_BLOCK_SKIP

#endif /* SEGGER_RTT_CONF_H */
