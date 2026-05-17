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
#include "pfb_itm.h"   /* ITM_EVENT32 — bare-metal SWO trace for ei_malloc sizing */

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
#define EI_POOL_SIZE  (88u * 1024u)   /* 88 KB of SRAM1.
                                         2026-05-17 night (Phase 2a continuous
                                         wake-word arch): shrunk from 120 KB
                                         to 88 KB to free 32 KB at the top of
                                         SRAM1 for s_wakeWindow[16000] int16_t
                                         (1 second of PCM at 16 kHz) declared
                                         in voice_recorder.c at 0x30016000.
                                         Pool peak observed with LIFO ei_free:
                                         17 KB. 88 KB still gives 5× margin.
                                         Previous comment (preserved for full
                                         context): 2026-05-17 evening shrunk
                                         128 KB -> 120 KB to free 8 KB at the
                                         TOP of SRAM1 so we could move
                                         s_DfsdmBuf back to D2 SRAM1
                                         (it was relocated to SRAM2 during EI
                                         integration; suspected to be the
                                         audio-amplitude regression because
                                         SRAM2's MPU/clock config may differ
                                         from SRAM1 in a way DMA can't tolerate).
                                         Peak observed with LIFO ei_free: 17 KB.
                                         120 KB still gives 7× margin.
                                         Previously: grown 96 → 128 KB.
                                         New EON-tuned MFCC model (_51 + _52)
                                         from Studio EON Tuner still hit
                                         -1002 EIDSP_OUT_OF_MEM at 96 KB.
                                         tensor_arena was moved to AXI SRAM
                                         (0x24048280, 2.7 KB) so SRAM1 is
                                         exclusively for s_ei_pool — no
                                         collision with anything in the
                                         region. Read s_ei_pool_used /
                                         s_ei_pool_high via IAR Live Watch
                                         after first successful inference to
                                         see actual peak vs 131,072 ceiling.
                                         History: 96 KB worked for old _3
                                         MFCC model (62,976 peak), too small
                                         for new _51/_52 EON-tuned MFCC. */
#pragma data_alignment = 32
#pragma location = ".sram1"
__no_init static uint8_t s_ei_pool[EI_POOL_SIZE];
static volatile size_t   s_ei_pool_used = 0u;
static volatile size_t   s_ei_pool_high = 0u;   /* peak high-water mark for tuning */

/* ── ITM allocation tracing ───────────────────────────────────────────────
 * Set to 0 to silence the per-allocation ITM stream once pool sizing is
 * resolved (the classifier itself keeps running — this only gates tracing).
 * Ports (view in IAR: View → SWO Trace, hex view):
 *   port 2 ← aligned size of each successful allocation
 *   port 3 ← s_ei_pool_used running total after each allocation
 *   port 4 ← the aligned size that overflowed the pool (ei_malloc → nullptr)
 * NOTE: enable ports 2/3/4 in IAR SWO Trace → Stimulus ports, else the
 * ITM_EVENT32 macro silently no-ops (it checks TER before writing). */
#define WAKE_EI_ITM_TRACE  1

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
 * definitions must match the header's linkage exactly.
 *
 * ──────────────────────────────────────────────────────────────────────────
 * 2026-05-17: UPGRADED FROM PURE BUMP → STACK ALLOCATOR (LIFO bump-back).
 * ──────────────────────────────────────────────────────────────────────────
 * Why: the previous no-op ei_free caused s_ei_pool_used to accumulate every
 * scratch buffer EI's MFCC code allocated during a single classify pass,
 * even though EI's code follows the standard pattern:
 *     scratch1 = ei_malloc(N); use; ei_free(scratch1);
 *     scratch2 = ei_malloc(M); use; ei_free(scratch2);
 * With a no-op free, we observed s_ei_pool_used = 130,944 B (overflowed
 * 128 KB) when Studio claimed only 4.7 KB peak live memory was needed.
 * That ~28× discrepancy was OUR bug, not Studio's estimate.
 *
 * Fix: each allocation gets a 32-byte header that records its aligned size.
 * In ei_free, if the freed block ends exactly at the current bump pointer
 * (i.e. it was the most-recently-allocated chunk), rewind the bump pointer
 * to reclaim its space. If freed out of order (rare), the chunk leaks until
 * the next ei_pool_reset() — bounded by pool size, never crashes.
 *
 * Still Hard Rule #1 compliant — no real malloc/free, just smarter slicing
 * of the same static s_ei_pool[].
 * ──────────────────────────────────────────────────────────────────────── */

/* 32-byte header per allocation. Layout in pool:
 *   [4B aligned-size][28B pad to keep user ptr 32-aligned][N bytes user data]
 * Header lives at user_ptr - 32. */
#define EI_HDR_BYTES  32u

__attribute__((used)) void *ei_malloc(size_t size)
{
    size_t aligned = (size + 31u) & ~31u;       /* 32-byte alignment for cache/CMSIS */
    size_t total   = aligned + EI_HDR_BYTES;    /* user data + header */

    if (s_ei_pool_used + total > EI_POOL_SIZE) {
#if WAKE_EI_ITM_TRACE
        /* port 4: the allocation that overflowed the pool — the "killer". */
        ITM_EVENT32(4, (uint32_t)aligned);
#endif
        return nullptr;
    }

    /* Write the header (size) at the start of the new block, then return
     * the address EI_HDR_BYTES bytes past it. */
    uint8_t  *header = &s_ei_pool[s_ei_pool_used];
    *(uint32_t *)header = (uint32_t)aligned;
    uint8_t  *p      = header + EI_HDR_BYTES;
    s_ei_pool_used  += total;

    /* Track real-time peak (was only updated at ei_pool_reset() before —
     * which meant peak never registered if classifier failed mid-pass). */
    if (s_ei_pool_used > s_ei_pool_high) {
        s_ei_pool_high = s_ei_pool_used;
    }

#if WAKE_EI_ITM_TRACE
    /* port 2: this allocation's aligned size; port 3: running pool total. */
    ITM_EVENT32(2, (uint32_t)aligned);
    ITM_EVENT32(3, (uint32_t)s_ei_pool_used);
#endif
    return (void *)p;
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
    /* LIFO bump-back. If ptr is the most-recently-allocated chunk, rewind
     * s_ei_pool_used to reclaim its space. Otherwise leak it (bounded by
     * pool — reclaimed at next ei_pool_reset()). */
    if (ptr == nullptr) return;

    uint8_t *user = (uint8_t *)ptr;

    /* Sanity: ptr must be inside our pool, past room for a header. */
    if (user < &s_ei_pool[EI_HDR_BYTES] ||
        user >= &s_ei_pool[EI_POOL_SIZE]) {
        return;  /* foreign pointer — ignore */
    }

    uint8_t  *header  = user - EI_HDR_BYTES;
    uint32_t  aligned = *(uint32_t *)header;
    size_t    total   = (size_t)aligned + EI_HDR_BYTES;

    /* If this block ends exactly at the current bump pointer, it was the
     * most recent allocation → rewind. Otherwise an interleaving free —
     * leak it for now. */
    if (header + total == &s_ei_pool[s_ei_pool_used]) {
        s_ei_pool_used -= total;
    }
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
