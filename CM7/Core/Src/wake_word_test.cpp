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
#include "voice_recorder.h"  /* Phase 3: WakeWord_GetWindow, voiceRecTaskHandle, RecState_t */

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

/* Phase 3 access to the global recording state — declared in voice_recorder.c.
 * Not in voice_recorder.h because it's a project-internal volatile global, but
 * we need it here to gate the continuous classifier (skip while a recording
 * is in flight). */
extern "C" {
    extern volatile RecState_t g_State;
}

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
#define EI_POOL_SIZE  (56u * 1024u)   /* 56 KB of SRAM1.
                                         2026-05-17 late night (Phase 3 build
                                         fix): shrunk further from 88 KB to
                                         56 KB to also free 32 KB for
                                         s_continuousBuf[16000] (classifier
                                         input snapshot, this file) at
                                         0x3000E000. Without this move the
                                         buffer landed in AXI SRAM which is
                                         saturated at 248/228 KB requested vs
                                         available -> Lp011 linker error.
                                         Pool peak observed with LIFO ei_free:
                                         17 KB. 56 KB still gives 3.3× margin.
                                         Previous step (Phase 2a 88 KB) was OK
                                         too — we only added s_continuousBuf
                                         since then. Earlier history:
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

/* ═══════════════════════════════════════════════════════════════════════════
 * Phase 3 (2026-05-17): WakeWord_OnIdleContinuous — always-on wake-word
 * listener. Runs from the idle hook every ~500 ms, reads the latest 1 sec
 * of audio from voice_recorder.c's rolling window (s_wakeWindow), runs the
 * EI classifier, and on `HEY NOA > threshold` triggers a button-equivalent
 * recording via xTaskNotify(voiceRecTaskHandle, 2, eSetBits).
 *
 * Phase 3 is the actual "Hey Noa" trigger — no button required. Coexists
 * with the existing button-test path (WakeWordTest_OnIdle) which classifies
 * the post-recording g_AudioBuf for benchmark/comparison.
 *
 * State gates (skip the classify pass if any of these are true):
 *   - cooldown timer not yet expired (1.5 s after last detection)
 *   - g_State != REC_IDLE (currently recording or saving)
 *   - <500 ms since last classify (rate limit at 2 Hz to leave CPU for UI)
 *
 * Memory: a 32 KB int16_t copy buffer for the classifier input (separate
 * from s_wakeWindow because run_classifier may read samples in any order
 * via the get_data callback, and we want a stable snapshot independent of
 * the rolling write head). Lives in default .bss (AXI SRAM).
 *
 * Trigger value: notify value 2 distinguishes wake-word from button
 * (button ISR uses value 1). VoiceRecTask currently treats both
 * identically; future code may want to differentiate (e.g. add a 500 ms
 * pre-record delay for wake-word so STT doesn't see the wake word itself).
 * ═══════════════════════════════════════════════════════════════════════════ */
#ifndef WAKEWORD_THRESHOLD_PCT
/* 2026-05-17: dropped 70 -> 50 after first round of testing. First detection
 * hit 96% but subsequent attempts were only 4-16% (rolling-window slicing
 * cuts utterances across edges, lowering confidence). 50% still firmly
 * favors HEY NOA over the other two labels but tolerates partial captures. */
#define WAKEWORD_THRESHOLD_PCT  50u
#endif
#ifndef WAKEWORD_CLASSIFY_INTERVAL_MS
/* 2026-05-17: dropped 500 -> 250 ms. With a 1-sec window and 250 ms cadence
 * any 700 ms "Hey Noa" utterance is fully contained in at least 2 of the 4
 * windows that cover it — gives the model multiple chances to recognize. */
#define WAKEWORD_CLASSIFY_INTERVAL_MS  250u
#endif
#ifndef WAKEWORD_COOLDOWN_MS
#define WAKEWORD_COOLDOWN_MS  1500u
#endif

/* Classifier input buffer — copied from rolling window before each inference.
 * Pinned to D2 SRAM1 at 0x3000E000 (in the 32 KB gap freed by shrinking
 * s_ei_pool from 88 KB -> 56 KB). AXI was saturated, so this can't live in
 * default .bss. CPU-only access (no DMA), so D2 SRAM1's Non-Cacheable region
 * isn't strictly required — but keeping all wake-word buffers in SRAM1 keeps
 * the placement audit story coherent and avoids any future cache surprises.
 *
 * Updated SRAM1 layout post Phase 3:
 *   0x30000000-0x3000DFFF  56 KB   s_ei_pool        (was 88 KB)
 *   0x3000E000-0x30015FFF  32 KB   s_continuousBuf  NEW (Phase 3)
 *   0x30016000-0x3001DFFF  32 KB   s_wakeWindow     (Phase 2a)
 *   0x3001E000-0x3001E1FF  512 B   s_DfsdmBuf       (DFSDM DMA target)
 *   0x3001E200-0x3001FFFF  ~7.5KB  FREE             safety margin
 */
#pragma data_alignment = 32
#pragma location = 0x3000E000
__no_init static int16_t s_continuousBuf[EI_CLASSIFIER_RAW_SAMPLE_COUNT];

extern "C" void WakeWord_OnIdleContinuous(void)
{
    static uint32_t s_lastClassifyMs  = 0u;
    static uint32_t s_cooldownUntilMs = 0u;

    uint32_t now = HAL_GetTick();

    /* Cooldown after a detection — gives VoiceRecTask time to transition to
     * REC_RECORDING (then the g_State gate below naturally suppresses re-
     * triggers until the recording + save cycle completes). */
    if (now < s_cooldownUntilMs) return;

    /* Skip if currently recording or saving. CPU is busy + g_AudioBuf is in
     * use for the captured-audio path. Wake-word classification can resume
     * after SDWriteTask sets g_State back to REC_IDLE. */
    if (g_State != REC_IDLE) return;

    /* Rate limit. */
    if ((now - s_lastClassifyMs) < WAKEWORD_CLASSIFY_INTERVAL_MS) return;
    s_lastClassifyMs = now;

    /* Snapshot the most-recent 1 sec from the rolling window. */
    size_t got = WakeWord_GetWindow(s_continuousBuf,
                                    (size_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT);
    if (got != (size_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT) return;

    /* Point the existing signal callback at our snapshot buffer + run. */
    signal_t signal;
    signal.total_length = EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    signal.get_data     = &wake_ei_get_signal_data;

    ei_impulse_result_t result;
    memset(&result, 0, sizeof(result));

    ei_pool_reset();
    s_classifyBuf = s_continuousBuf;
    __DSB();

    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    if (r != EI_IMPULSE_OK) {
        /* Log failures sparingly — once per 5 sec — so we know if EI broke. */
        static uint32_t s_lastFailMs = 0u;
        if ((now - s_lastFailMs) > 5000u) {
            s_lastFailMs = now;
            RLOG("[WAKE] EI fail rc=", (uint32_t)r);
        }
        return;
    }

    /* Find HEY NOA confidence. Labels are exactly "HEY NOA" / "Noise" /
     * "unknown" per observed runs — match by first character to avoid
     * any accidental match on lowercase label variants. */
    uint32_t heyNoaPct = 0u;
    for (uint32_t i = 0u; i < (uint32_t)EI_CLASSIFIER_LABEL_COUNT; i++) {
        const char *lbl = result.classification[i].label;
        if (lbl && (lbl[0] == 'H' || lbl[0] == 'h')) {  /* "HEY NOA" */
            heyNoaPct = (uint32_t)(result.classification[i].value * 100.0f);
            break;
        }
    }

    /* Heartbeat: every 10th classify (≈ every 5 sec) print stats to confirm
     * the continuous path is alive AND the rolling window has real audio.
     *
     * Four numbers per heartbeat (4 RLOG lines — RLOG only logs one uint32):
     *   cnt   = total classifies since boot. Stuck = function isn't running.
     *   head  = s_wakeWindowHead. Δ between heartbeats should be ~16000
     *           (16 kHz × 1 sec); 0 means DMA/drainer is dead.
     *   peak  = max|sample| in this classify's input snapshot. Silence ~10-100;
     *           normal speech ~1000-5000. Tells us mic is actually capturing.
     *   pct   = HEY NOA confidence 0..100. Low = mic OK but no wake word said.
     */
    static uint32_t s_classifyCnt = 0u;
    s_classifyCnt++;
    if ((s_classifyCnt % 10u) == 0u) {
        /* Compute peak amplitude in the just-classified snapshot. */
        int32_t peakAbs = 0;
        for (uint32_t i = 0u; i < (uint32_t)EI_CLASSIFIER_RAW_SAMPLE_COUNT; i++) {
            int32_t v = s_continuousBuf[i];
            if (v < 0) v = -v;
            if (v > peakAbs) peakAbs = v;
        }
        RLOG("[WAKE-HB] cnt=",  s_classifyCnt);
        RLOG("[WAKE-HB] head=", WakeWord_GetWindowHead());
        RLOG("[WAKE-HB] peak=", (uint32_t)peakAbs);
        RLOG("[WAKE-HB] pct=",  heyNoaPct);
    }

    if (heyNoaPct >= WAKEWORD_THRESHOLD_PCT) {
        RLOG("[WAKE] HEY NOA detected pct=", heyNoaPct);

        /* Trigger recording (same notification the PC13 ISR uses, but value
         * 2 = wake-word instead of value 1 = button). VoiceRecTask checks
         * the notify value only to differentiate later if it cares; for
         * now it starts a 3-sec recording either way. */
        if (voiceRecTaskHandle != NULL) {
            xTaskNotify(voiceRecTaskHandle, 2u, eSetBits);
        }

        s_cooldownUntilMs = now + WAKEWORD_COOLDOWN_MS;
    }
}

#endif /* WAKE_WORD_TEST */
