/* wake_word_test.cpp — quick-test bench for Edge Impulse "Hey Noa" classifier.
 *
 * See wake_word_test.h for the rationale + API contract.
 *
 * The whole file is gated by `#ifdef WAKE_WORD_TEST`. When the symbol is not
 * defined the .cpp compiles to zero code, so the IAR project can keep this
 * file in its compile list without affecting the binary. Default build = no
 * behaviour change.
 *
 * Implementation note — why we override vApplicationGetIdleTaskMemory:
 *   The CMSIS-RTOS wrapper (Middlewares/.../cmsis_os2.c) provides a __WEAK
 *   default that gives the idle task only `configMINIMAL_STACK_SIZE` words
 *   (= 128 words = 512 bytes). TF-Lite Micro inference needs several KB on
 *   the stack. We override the weak symbol with our own version that supplies
 *   a 14 KB stack pinned to D2 SRAM1 (so it doesn't eat AXI, which is near
 *   full). The override is unconditional when WAKE_WORD_TEST is defined; not
 *   compiled in otherwise, so the weak default still applies.
 */
#ifdef WAKE_WORD_TEST

#include "wake_word_test.h"
#include "log_mutex.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/* Edge Impulse public API. EI_PORTING_IAR=1 + EI_CLASSIFIER_ALLOCATION_STATIC=1
 * must already be set in the IAR project's Preprocessor → Defined symbols. */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

/* ── Idle task stack override ─────────────────────────────────────────────── */

/* 14 KB stack for the idle task — pinned to D2 SRAM1 at 0x30010000 (64 KB past
 * `s_DfsdmBuf`) so the increase doesn't pressure AXI SRAM (already ~498/512 KB
 * used per docs/Map.md). 3584 StackType_t words × 4 bytes = 14 336 bytes. */
#define WAKE_EI_IDLE_STACK_WORDS  3584u

/* Moved from D2 SRAM1 @ 0x30010000 to D2 SRAM2 @ 0x30038000 (free space past
 * g_AudioBuf which ends at 0x30037700) so the 80 KB s_ei_pool can use the
 * full upper half of D2 SRAM1. */
#pragma data_alignment = 32
#pragma location = 0x30038000
__no_init static StackType_t s_idleStack[WAKE_EI_IDLE_STACK_WORDS];

static StaticTask_t s_idleTaskCB;

/* Override of the __WEAK default in cmsis_os2.c. FreeRTOS calls this exactly
 * once during xTaskCreateStatic for the idle task when
 * configSUPPORT_STATIC_ALLOCATION == 1. Our version replaces the 512-byte
 * default with the 14 KB buffer above. */
extern "C" void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                              StackType_t **ppxIdleTaskStackBuffer,
                                              uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer   = &s_idleTaskCB;
    *ppxIdleTaskStackBuffer = &s_idleStack[0];
    *pulIdleTaskStackSize   = WAKE_EI_IDLE_STACK_WORDS;
}

/* ── Trigger payload ─ written by WakeWordTest_Trigger, read by the idle hook */
static volatile bool                  s_classifyPending = false;
static volatile const int16_t        *s_classifyBuf     = nullptr;
static volatile size_t                s_classifyCount   = 0u;

/* ── signal_t callback — int16 PCM → float (-1.0 to +1.0) on demand ───────── */
static int wake_ei_get_signal_data(size_t offset, size_t length, float *out_ptr)
{
    const int16_t *src = (const int16_t *)s_classifyBuf;
    if (src == nullptr) return -1;
    for (size_t i = 0; i < length; i++) {
        out_ptr[i] = ((float)src[offset + i]) / 32768.0f;
    }
    return 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

extern "C" void WakeWordTest_Trigger(const int16_t *samples, size_t count)
{
    if (samples == nullptr || count == 0u) return;
    s_classifyBuf   = samples;
    s_classifyCount = count;
    __DSB();
    s_classifyPending = true;   /* armed — next idle hook tick will process */
}

/* ── Static bump allocator overriding EI's weak ei_malloc/ei_calloc/ei_free ──
 *
 * EI's MFCC pipeline does a flurry of short-lived allocations during each
 * run_classifier() invocation (FFT scratch, filterbank temps, etc.). The
 * default ei_malloc in porting/iar/ei_classifier_porting.cpp is __WEAK and
 * just calls standard malloc — which violates Hard Rule #1 (no dynamic
 * allocation) and would exhaust the 4 KB DLib heap (saw EIDSP_OUT_OF_MEM,
 * code -1002, on the first attempt).
 *
 * Our solution: a bump allocator over a 40 KB pool in D2 SRAM1. Allocations
 * are O(1) pointer increments; "free" is a no-op; the pool is RESET to empty
 * before each run_classifier() call (see WakeWordTest_OnIdle above where we
 * call ei_pool_reset()). Zero fragmentation, zero runtime malloc, fully
 * deterministic. Hard Rule #1 compliant.
 *
 * Sizing: 40 KB is overkill for our compact keyword model. Resize down
 * (or up) once we observe peak high-water mark via s_ei_pool_high.
 */
#define EI_POOL_SIZE  (96u * 1024u)   /* 96 KB pool in D2 SRAM1. After EON+int8
                                         export, observed s_ei_pool_used = 62,976
                                         on-device (cut from 110 KB pre-EON — 43%
                                         reduction). 96 KB gives ~33 KB margin over
                                         the observed peak. 96 + 2.2 KB tensor_arena
                                         = 98.2 KB → fits in 128 KB SRAM1 with
                                         ~30 KB region safety margin too. */
#pragma data_alignment = 32
#pragma location = ".sram1"
__no_init static uint8_t s_ei_pool[EI_POOL_SIZE];
static volatile size_t   s_ei_pool_used = 0u;
static volatile size_t   s_ei_pool_high = 0u;   /* peak high-water mark for tuning */

/* Reset the pool — call once before each run_classifier(). */
static void ei_pool_reset(void)
{
    if (s_ei_pool_used > s_ei_pool_high) {
        s_ei_pool_high = s_ei_pool_used;
    }
    s_ei_pool_used = 0u;
}

/* Override the weak defaults in porting/iar/ei_classifier_porting.cpp.
 * NOTE: NO `extern "C"` — the header (ei_classifier_porting.h:197/231/258)
 * declares these with C++ linkage unless EI_C_LINKAGE == 1 is set. Our
 * definitions must match the header's linkage exactly. */
__attribute__((used)) void *ei_malloc(size_t size)
{
    size_t aligned = (size + 31u) & ~31u;   /* 32-byte alignment for cache safety */
    if (s_ei_pool_used + aligned > EI_POOL_SIZE) {
        return nullptr;
    }
    void *p = &s_ei_pool[s_ei_pool_used];
    s_ei_pool_used += aligned;
    return p;
}

__attribute__((used)) void *ei_calloc(size_t nitems, size_t size)
{
    size_t total = nitems * size;
    void *p = ei_malloc(total);
    if (p != nullptr) {
        memset(p, 0, total);
    }
    return p;
}

__attribute__((used)) void ei_free(void *ptr)
{
    /* Bump allocator — free is intentionally a no-op. The pool is reset
     * en masse at the start of each inference via ei_pool_reset(). */
    (void)ptr;
}

/* Called from vApplicationIdleHook (freertos.c). Must not block. Runs the
 * classifier inline on the idle task's stack. Returns immediately if no
 * request is pending. */
extern "C" void WakeWordTest_OnIdle(void)
{
    if (!s_classifyPending) return;

    /* Snapshot trigger payload locally, then clear the flag so a re-trigger
     * during inference doesn't get lost (we'll re-run on the next idle tick). */
    const int16_t *buf   = (const int16_t *)s_classifyBuf;
    size_t         count = s_classifyCount;
    s_classifyPending    = false;
    __DSB();

    if (buf == nullptr || count < (size_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT) {
        RLOG("[EI] skip invalid buf count=", (uint32_t)count);
        return;
    }

    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data     = &wake_ei_get_signal_data;

    ei_impulse_result_t result;
    memset(&result, 0, sizeof(result));

    /* Reset the bump pool before inference so MFCC's allocations start at offset 0. */
    ei_pool_reset();

    RLOG("[EI] classify START samples=", (uint32_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT);
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    if (r != EI_IMPULSE_OK) {
        RLOG("[EI] FAILED rc=", (uint32_t)r);
        return;
    }

    /* Log per-label probability as percent (0-100) to avoid float printf. */
    for (uint32_t i = 0u; i < (uint32_t)EI_CLASSIFIER_LABEL_COUNT; i++) {
        uint32_t pct = (uint32_t)(result.classification[i].value * 100.0f);
        const char *label = result.classification[i].label;
        /* Print "label_name <pct>" via the raw queue API — RLOG can't take strings. */
        Log_ToQueue(label ? label : "[EI] ?", pct);
    }

    RLOG("[EI] dsp_ms=", (uint32_t)result.timing.dsp);
    RLOG("[EI] inf_ms=", (uint32_t)result.timing.classification);
}

#endif /* WAKE_WORD_TEST */
