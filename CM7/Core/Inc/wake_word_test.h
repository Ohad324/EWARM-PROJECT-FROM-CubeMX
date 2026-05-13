/* wake_word_test.h — quick-test bench for Edge Impulse "Hey Noa" classifier.
 *
 * Purpose:
 *   After a button-press recording finishes (3 s of int16_t PCM in g_AudioBuf),
 *   feed the first 1 second (16000 samples) into run_classifier() and log the
 *   per-label probabilities via RLOG. Lets us verify the model classifies
 *   correctly on STM32H7 hardware BEFORE wiring up continuous DMA + wake-word.
 *
 * Compile-time gate:
 *   Define WAKE_WORD_TEST=1 in IAR Preprocessor → Defined symbols to enable.
 *   Without the define, this header is still safe to include (forward-declares
 *   the API as no-ops) but no implementation is built and no callers run.
 *
 * Execution model:
 *   Runs inside the FreeRTOS idle hook (vApplicationIdleHook). That means the
 *   classifier executes only when no other task is ready — by definition the
 *   "system is quiet" state. The idle task's stack is bumped to 14 KB (in D2
 *   SRAM1) by overriding the weak vApplicationGetIdleTaskMemory provided by
 *   the CMSIS-RTOS wrapper.
 */
#ifndef WAKE_WORD_TEST_H
#define WAKE_WORD_TEST_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Schedule classification on `samples[0..count-1]` (count must be >= 16000).
 * Non-blocking: just records the buffer pointer + arms a flag. The next time
 * the idle hook runs, it picks up the flag and runs run_classifier() inline.
 * Caller must keep `samples` alive until results appear in the RTT log
 * (typically a few hundred ms). Safe to call from any task (NOT from ISR). */
void WakeWordTest_Trigger(const int16_t *samples, size_t count);

/* Called from vApplicationIdleHook. If a classification is armed, runs the
 * model and logs results, then clears the flag. Returns immediately if no
 * request is pending. Internal — declared here so freertos.c can call it. */
void WakeWordTest_OnIdle(void);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_TEST_H */
