# Plan — Wake-Word Trigger via Edge Impulse "Hey Noa"

**Filed:** 2026-05-11 evening
**Baseline:** commit `0281121` / tag `complete-end-to-end-no-edge-impulse`
**Goal:** Replace the blue PC13 push-button trigger with an Edge Impulse keyword spotter that fires when the user says **"Hey Noa"**. The existing recording / SD / UART / GCS / STT pipeline downstream is unchanged.

---

## 1. Current state of Edge Impulse in the project

**The SDK is vendored but completely unbuilt and unlinked.** Confirmed by:

```
$ grep -iE "edge.impulse|ei_run|EI_CLASSIFIER|tflite_learn|ei_classifier"
     EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map
(no matches)
```

So the answer to **"where is EI in the `.map`?"** is: **nowhere yet.** It's all source code sitting on disk waiting to be added to the IAR project's compile list.

### What IS in the repo (committed `e050ea4`)

```
Edge Impulse/
├── CMakeLists.txt                 — for Linux/PC build, not used by IAR
├── README.txt
├── ohad3240-project-1-cpp-mcu-v1-impulse-#1.zip   — original download
├── edge-impulse-sdk/              — the SDK proper (large, ~thousands of files)
│   ├── classifier/                — ei_run_classifier.h + types (the API the firmware calls)
│   ├── dsp/                       — MFCC + spectrogram feature extractor
│   ├── tensorflow/                — TF-Lite Micro kernels
│   ├── CMSIS/                     — vendored ARM CMSIS DSP/NN (already in project — potential conflict)
│   ├── porting/                   — platform glue (mbed / zephyr / silabs / nordic / etc.)
│   └── third_party/
├── model-parameters/
│   ├── model_metadata.h           — defines like EI_CLASSIFIER_FREQUENCY=16000, LABEL_COUNT=2
│   └── model_variables.h          — categories: { "HEY NOA", "Noise" }
└── tflite-model/
    ├── tflite_learn_985318_3_compiled.cpp   — the actual generated inference code
    ├── tflite_learn_985318_3_compiled.h
    └── trained_model_ops_define.h
```

### Key model parameters (from `model_metadata.h`)

| Setting | Value | Implication |
|---|---|---|
| `EI_CLASSIFIER_FREQUENCY` | 16 000 Hz | Matches our DFSDM PCM rate exactly ✓ |
| `EI_CLASSIFIER_RAW_SAMPLE_COUNT` | 16 000 | 1-second audio window |
| `EI_CLASSIFIER_INTERVAL_MS` | 0.0625 | 16 kHz sample interval |
| `EI_CLASSIFIER_LABEL_COUNT` | 2 | `"HEY NOA"` and `"Noise"` |
| `EI_CLASSIFIER_NN_INPUT_FRAME_SIZE` | 650 | MFCC feature vector size |
| `EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW` | (in metadata) | continuous inference cadence |

The frequency match is the most important fact — we don't need any resampling. The same `g_AudioBuf` samples used for cloud STT can be fed directly into EI.

---

## 2. The architectural question (decide before coding)

There are two distinct ways to integrate EI on this hardware. Both are viable, but they have different blast radius and CPU cost.

### Option A — **Always-listening, sliced inference**

A new `WakeWordTask` runs the EI classifier on a **rolling 1-second window** of audio, with the window advanced every 250 ms or 500 ms (so a phrase that crosses the slice boundary still gets classified). When the classifier returns `"HEY NOA"` with confidence > threshold, the task generates the **same notification** that `HAL_GPIO_EXTI_Callback` currently sends — `Music_RequestTestThumbFromISR` or the recording-start signal to `VoiceRecTask`.

**Pros:**
- Hands-free, real keyword spotting
- One-shot integration; once it works, nothing more to tune
- Path is exactly what Edge Impulse documents as its primary use case

**Cons:**
- CPU cost: every 250 ms we run ~1-2 ms of MFCC + ~3-5 ms of TF-Lite inference on M7. Headroom check needed.
- ~50-100 KB of RAM for the inference arena (TF-Lite scratch buffer) — placement matters per Hard Rule #3
- Adds ~150-200 KB of Flash code (TF-Lite kernels + MFCC tables + model weights)

### Option B — **Two-phase: voice activity detector + EI confirm**

A cheap energy/zero-crossing VAD runs continuously. When it detects "voice-like" audio, it triggers a single 1-second EI inference. If EI returns `"HEY NOA"`, fire the trigger.

**Pros:**
- ~10x lower average CPU load (EI runs only on actual voice events)
- Smaller scratch buffer needed (one shot, not pipelined)

**Cons:**
- VAD threshold tuning required — too sensitive = false EI invocations; too strict = missed wake words
- The first ~100 ms of the wake word may be lost while VAD wakes up
- More moving parts to debug

**Recommendation:** start with **Option A**. The M7 at 400 MHz has plenty of headroom (current IDLE task shows >134M cycles free per IAR Tasks view). Option B is a fallback if profiling shows Option A is too expensive.

---

## 3. Implementation plan — Option A

Done in **5 sequential steps**, one Build + Flash + Test per step (per the small-step rule). If any step regresses the working system, revert before continuing.

### Step 1 — Add EI source files to the IAR project (no behavior change yet)

The IAR project file (`STM32H747I-DISCO.ewp`) must list every `.c` / `.cpp` file in its compile group. Per memory `feedback_no_ewp_edits.md` this MUST be done through the IAR IDE, not by editing the `.ewp` directly.

**Files to add** (verify each against current SDK contents):

| Group in IAR | Files |
|---|---|
| `Edge Impulse / classifier` | none directly — `classifier/` is header-only API |
| `Edge Impulse / dsp` | `dsp/numpy.cpp`, `dsp/spectral/feature.cpp`, `dsp/speechpy/feature.cpp` (audio processing) |
| `Edge Impulse / tensorflow` | The full `tensorflow/lite/micro/` tree (large — start with `kernels/` it needs + `micro_*.cpp`) |
| `Edge Impulse / model` | `tflite-model/tflite_learn_985318_3_compiled.cpp` |
| `Edge Impulse / porting` | `porting/iar/` if it exists, else `porting/posix/ei_classifier_porting.cpp` adapted for FreeRTOS |

**Include paths to add** to compiler options (CCIncludePath2 in IAR):
- `$PROJ_DIR$/../Edge Impulse`
- `$PROJ_DIR$/../Edge Impulse/edge-impulse-sdk`

**Preprocessor defines:**
- `EI_PORTING_GENERIC=1` (or `EI_PORTING_IAR=1` if a porting layer exists)
- `EIDSP_SIGNAL_C_FN_POINTER=1` (no exceptions, no STL allocator surprises)

**Expected outcome:** build succeeds, `.map` shows ~150-200 KB of new `.text` code in flash. Look for `ei_run_classifier` and `tflite_learn_985318_*` symbols in the map. **AXI / `.bss` should grow by only the size of the inference arena (~40-100 KB) if any static arena is declared.**

**Risk:** the SDK's vendored CMSIS may collide with the project's existing CMSIS at link time (duplicate symbols). Mitigation: exclude `Edge Impulse/edge-impulse-sdk/CMSIS/` from the compile list — the firmware already has CMSIS from `Drivers/CMSIS/`. Verify with `--whole-archive` warnings, fix by file-exclusion.

**Verification:** firmware still boots, LCD still renders, button still triggers a recording, STT still works. Nothing calls EI yet — this is a link test only.

### Step 2 — Static-allocate the EI inference arena

EI's TF-Lite Micro needs a contiguous scratch buffer (the "arena"). The size depends on the model; for a 16 kHz keyword model it's typically 30-80 KB.

**Action:**
1. Find the arena size constant the model expects (likely defined in `tflite-model/tflite_learn_985318_3_compiled.cpp` or via `EI_CLASSIFIER_TFLITE_ARENA_SIZE` macro).
2. Declare the arena as a **static buffer with `#pragma location = ".sram1"`** per Hard Rule #3. D2 SRAM1 has 127 KB free (only `s_DfsdmBuf` at the start). Use the **end** of SRAM1 (`0x30010000` onward) to keep distance from DFSDM DMA.
3. Configure EI to use this fixed arena (not malloc) — the SDK has a `ei_classifier_aligned_malloc` hook we override.

**Why D2 SRAM1 not AXI:** AXI is 97% full after the LCD re-enable. Adding 40+ KB of arena there would refile [BUG-002](BUG-002_AXI_OVERFLOW_LCD_ENABLE.md). D2 SRAM1 is empty and DMA-reachable (though EI's arena doesn't need DMA — CPU-only access). It's also in the same domain as the audio path we're feeding from.

**Verification:** `.map` shows `ei_arena[]` at `0x30010000`-ish, and the AXI summary doesn't grow.

### Step 3 — Create `WakeWordTask` that consumes from `g_AudioBuf` continuously

Right now `g_AudioBuf` is filled only during the 3-second post-button-press window. For continuous wake-word listening, **DFSDM must run continuously**, with the buffer used as a circular ring.

Two ways to handle this:

**3a — Repurpose `g_AudioBuf` as a rolling ring** (preferred — no new buffer)
- Modify `VoiceRecTask` so it starts DFSDM DMA at boot in `DMA_CIRCULAR` mode (currently `DMA_NORMAL`).
- `StoreDmaChunk` already runs in the DMA half/complete ISRs and writes into `g_AudioBuf`. Make the index wrap-around (`g_SampleCount = (g_SampleCount + chunk) % 48000`).
- `WakeWordTask` reads the most recent 1 second (16000 samples) from this ring every ~250 ms.

**3b — Allocate a separate 1-second wake-word ring**
- New `uint16_t s_wakeRing[16000];` (32 KB) in D2 SRAM1.
- DFSDM has two DMA streams? No — only one is wired (DMA1_Stream1). So this option requires double-write: ISR copies into both `g_AudioBuf` and `s_wakeRing`. More CPU.

**Recommendation: 3a.** It's cleaner, and the only behavioral change is `DMA_NORMAL` → `DMA_CIRCULAR`. `VoiceRecTask`'s existing logic of "fill 48000 then stop" becomes "fill 48000 then keep going + signal SDWriteTask".

Once the wake-word triggers, `VoiceRecTask` enters its existing 3-second capture mode (now just a "freeze the next 48000 samples in the ring and hand off to SDWriteTask").

**Verification:** at idle the IRQ ring at `0x24050000` shows DMA1_Stream1 firing every 4 ms (128-sample chunks). `g_SampleCount` wraps. `[PCM]` Audio Quality Report values look reasonable.

### Step 4 — Wire WakeWordTask → EI → button-press signal

Inside `WakeWordTask` (run at priority `osPriorityLow` so it never preempts audio/UART):

```c
static void WakeWordTask(void* arg)
{
    signal_t signal;
    ei_impulse_result_t result;
    int16_t window[EI_CLASSIFIER_RAW_SAMPLE_COUNT];   // 16000 samples = 32 KB on stack!
    // ^ probably move to static .sram1 buffer instead

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));     // 4 Hz inference cadence

        // Copy the most recent 16000 samples out of the rolling g_AudioBuf
        // into the contiguous window buffer (handle ring wrap).
        snapshot_rolling_buffer(window);

        numpy::signal_from_buffer(window, EI_CLASSIFIER_RAW_SAMPLE_COUNT, &signal);
        EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
        if (r != EI_IMPULSE_OK) continue;

        float p_heynoa = result.classification[0].value;   // "HEY NOA" is index 0
        if (p_heynoa > 0.85f) {
            // Fire the same path as a button press
            Music_RequestTestThumbFromISR(NULL);   // or VoiceRec_StartFromISR()
            vTaskDelay(pdMS_TO_TICKS(3000));       // refractory period — don't re-fire
        }
    }
}
```

The 16000-sample window MUST NOT live on the task stack (32 KB > all reasonable stack sizes on this project). Put it in `.sram1` as a static buffer.

**RLOG every classification result** so we can tune the threshold from the RTT log:

```c
RLOG("[EI] hey_noa=%.2f noise=%.2f", p_heynoa, p_noise);
```

**Verification:** RTT log shows `[EI]` lines every 250 ms with sensible probabilities. Saying "Hey Noa" out loud should push `hey_noa` above 0.85 within ~250-500 ms. Silence should keep it under 0.20.

### Step 5 — Replace (or supplement) the button trigger

In `HAL_GPIO_EXTI_Callback` keep the PC13 path as a **manual override** for testing; add `Music_RequestTestThumbFromISR` or `VoiceRec_StartFromISR` as the action from `WakeWordTask`.

**Optional debounce:** if EI fires repeatedly during a single utterance, add a 3-second refractory period in `WakeWordTask` (the `vTaskDelay(3000)` after firing).

**Verification end-to-end:**
1. Say "Hey Noa" → STM32 records 3 seconds of follow-up audio → SD write → UART → GCS upload → STT transcript
2. Say random other words → no trigger
3. PC13 button still works as manual fallback

---

## 4. Risks and known unknowns

| Risk | Severity | Mitigation |
|---|---|---|
| EI SDK's vendored CMSIS conflicts with project's CMSIS | High at Step 1 | Exclude `Edge Impulse/edge-impulse-sdk/CMSIS/` from compile, use project's CMSIS only |
| TF-Lite arena too large for D2 SRAM1 (>127 KB) | Medium | Profile arena size before Step 2. Fallback: D3 SRAM4 (60 KB unused after RTT) or move RTT back to AXI to free SRAM1 |
| Continuous DMA + LTDC scans causes AXI bus contention (FUIF regression) | Medium | If FUIF returns, lower WakeWordTask priority + add WFI yields per CLAUDE.md PFB section. PFB strip dispatch should be unaffected since g_AudioBuf is in D2 SRAM2, not AXI |
| MFCC + inference takes longer than 250 ms slice → backlog | Low | Reduce inference cadence to 500 ms, or use Option B (VAD-gated) |
| Model gives false positives on background noise | Low (training-side) | Adjust confidence threshold from 0.85; if still bad, retrain on the actual room acoustics with Edge Impulse Studio |
| Wake word retraining needs new SDK download | Medium | If true: pause integration, retrain in Edge Impulse Studio, re-vendor the SDK, restart at Step 1 |

---

## 5. Files to touch (final list)

| File | Change |
|---|---|
| `EWARM/STM32H747I-DISCO.ewp` | Add ~50-100 new compile units from `Edge Impulse/`. **Edit via IAR IDE only.** |
| `EWARM/stm32h747xx_flash_CM7.icf` | Possibly add `place in SRAM1_region { section .ei_arena };` if a named section is wanted (alternative to `#pragma location` absolute) |
| `CM7/Core/Src/voice_recorder.c` | Switch DFSDM DMA from `DMA_NORMAL` to `DMA_CIRCULAR`; adapt `StoreDmaChunk` to wrap the index; keep the 3-second hand-off semantics for the post-trigger window |
| `CM7/Core/Src/wake_word_task.c` *(NEW)* | The classifier loop, EI signal-from-buffer wiring, threshold check, trigger fire |
| `CM7/Core/Inc/wake_word_task.h` *(NEW)* | Public init + task entry |
| `CM7/Core/Src/main.c` | `xTaskCreate(WakeWordTask, ...)` with priority `osPriorityLow` and stack ~2048 words |
| `CM7/Core/Src/ei_porting_freertos.cpp` *(NEW)* | EI's required porting hooks: `ei_sleep`, `ei_read_timer_ms`, `ei_printf`, `ei_classifier_aligned_malloc/free` → static arena |
| `CLAUDE.md` | Append a SAVE STATE block at the end describing the wake-word integration outcome + how to disable for STT-only testing |

---

## 6. Verification gates (one per step — do not skip)

| After step | Smoke test | Pass criteria |
|---|---|---|
| 1 | Build + flash + boot | LCD renders, button still records, STT still returns transcript. `.map` shows ~150-200 KB new flash code |
| 2 | Build + map inspection | `ei_arena` symbol at `0x30010000`+ in `.map`. AXI free space unchanged |
| 3 | Boot + RTT | DFSDM DMA fires continuously, `g_SampleCount` wraps mod 48000, no overruns |
| 4 | RTT log | `[EI]` probability lines every 250 ms; saying "Hey Noa" lifts the first column above 0.85 |
| 5 | End-to-end | Voice wake → STM32 records → GCS → STT transcript. No SW change to NORA side |

---

## 7. Decision points before starting

Tomorrow morning, before touching code, decide:

1. **CMSIS strategy** — exclude EI's vendored CMSIS or namespace-fork it? **Recommendation: exclude, use project's CMSIS.**
2. **Continuous DFSDM** vs **gated DFSDM** — change DMA to circular and listen always? **Recommendation: continuous (Step 3a).**
3. **Inference cadence** — 250 ms (4 Hz, ~16 ms CPU/sec) or 500 ms (2 Hz, ~8 ms CPU/sec)? **Recommendation: 250 ms, easy to lower if profiling shows issues.**
4. **Trigger semantics on detection** — auto-record + STT (full pipeline), or just signal `Music_RequestTestThumbFromISR` for early testing? **Recommendation: start with the thumbnail signal (proves the trigger), then promote to full recording once detection is reliable.**

---

## 8. Out of scope (later sessions)

- Multi-keyword model (currently 2 labels: HEY NOA, Noise — fine for v1)
- Voice activity detector front-end (Option B above)
- Power-mode optimisation (low-power listening via DFSDM-only with M7 in Stop)
- Edge Impulse model retraining workflow

---

## 9. References

- [Edge Impulse "Running impulses" docs](https://docs.edgeimpulse.com/docs/edge-impulse-studio/deployment/running-your-impulse-locally) — `run_classifier()` API
- [CLAUDE.md Hard Rule #1 + #3](../CLAUDE.md) — static allocation + section attribution apply to EI arena
- [docs/MEMORY_MAP_AND_SW_BLOCKS.md](MEMORY_MAP_AND_SW_BLOCKS.md) — current memory layout
- [docs/Map_full.txt](Map_full.txt) — verbatim linker map at the pre-EI baseline (`a303dac`)
- Branch `test/inject-flasher-thumbnail-no-wifi`, tag `complete-end-to-end-no-edge-impulse` — known-good starting point
