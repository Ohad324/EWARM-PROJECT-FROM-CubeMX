# BUG-003 — Edge Impulse MFCC returns `-1002 EIDSP_OUT_OF_MEM` with EON+int8 export

## Header

| Field | Value |
|---|---|
| **ID** | BUG-003 |
| **Title** | Edge Impulse `run_classifier` fails with `-1002 EIDSP_OUT_OF_MEM` even after EON+int8 export and 96 KB static pool |
| **Status** | **OPEN** (carried into 2026-05-14 session) |
| **Severity** | Blocker for wake-word detection feature |
| **Component** | `wake_word_test.cpp` + Edge Impulse SDK + bump allocator pool |
| **Branch** | `test/inject-flasher-thumbnail-no-wifi` |
| **Last commit** | `0dbf89b` (SAVE STATE + 96 KB pool) — pushed to OneDrive origin |
| **GitHub mirror** | Out of sync (blocked by 720 MB blob in `f76f95e`, not by this bug) |
| **Opened** | 2026-05-13 (initial -1002 with 40 KB pool, pre-EON) |
| **Re-opened with EON+int8** | 2026-05-14 00:00 |
| **Author** | Ohad |

---

## Summary

After integrating the Edge Impulse SDK with EON Compiler + int8 quantization, the bump allocator's static pool overflows during MFCC feature extraction. The model's DSP block (Mel-Frequency Cepstral Coefficients on 1 second of 16 kHz audio) requests more cumulative memory through our overridden `ei_malloc` than the 96 KB pool can supply, so `ei_malloc` returns `nullptr` and EI's MFCC stage returns `EIDSP_OUT_OF_MEM (-1002)`. The audio capture, SD save, UART stream to NORA, and GCS upload pipelines all run cleanly — only the classifier itself fails.

---

## Reproduction

| Step | Action | Expected | Actual |
|---|---|---|---|
| 1 | Flash latest `.hex` (commit `0dbf89b`) | Boot, kernel starts, all tasks created | ✓ as expected |
| 2 | Press blue PC13 button | DFSDM records 3 sec; SD save; UART stream; GCS HTTP 200 | ✓ all happen — pipeline correct |
| 3 | Wait ~3 sec after recording | `[EI] classify START samples= 16000` → `HEY NOA <pct>` / `Noise <pct>` in Terminal I/O | ✗ instead get `ERR: MFCC failed (-1002)` + `ERR: Failed to run DSP process (-1002)` |
| 4 | Repeat steps 2-3 multiple times | Same result each time | ✓ — fully deterministic, not transient |

Three button-presses tonight produced identical `-1002` failures with no other side effects.

---

## Evidence (Terminal I/O excerpt)

```
[SD1:open ] cyc=4DF2DD83          ← recording captured + SD save
[SD2:alloc] cyc=4DFF492F
[SD3:hdr  ] cyc=4E0563AF
[SD4:pcm  ] cyc=4E06EA4D
[SD5:close] cyc=52CE4AFE
[UART:send] cyc=54FF8D7F          ← UART stream to NORA
ERR: MFCC failed (-1002)           ← THE BUG — emitted by EI SDK's ei_run_dsp.h
ERR: Failed to run DSP process (-1002)
[STR1:hdr ] cyc=5745CC5F           ← downstream pipeline continues normally
[STR2:rdy ] cyc=578100A1
[STR3:done] cyc=87362AF9
[PIPE:OK  ] cyc=8921B6A5           ← GCS upload HTTP 200 OK
```

The error `-1002` is `EIDSP_OUT_OF_MEM` defined in `Edge Impulse/edge-impulse-sdk/dsp/returntypes.h`. It is returned when the SDK calls our overridden `ei_malloc()` and we return `nullptr` because `s_ei_pool_used + size > EI_POOL_SIZE`.

---

## Pool sizing progression (chronological)

| Date / Time | Pool size | Outcome | Observed `s_ei_pool_used` | Notes |
|---|---|---|---|---|
| 2026-05-13 ~15:00 | 40 KB | -1002 | 38,336 | First EI integration, pre-EON |
| 2026-05-13 ~16:00 | 80 KB | -1002 | 81,664 | Slight increase, still overflow |
| 2026-05-13 ~17:00 | 96 KB | -1002 | 97,472 | Pre-EON peak — model genuinely needed more |
| 2026-05-13 ~18:00 | 108 KB | -1002 | 110,176 | Pre-EON, near overflow again |
| 2026-05-13 ~22:00 | 128 KB | Linker error (Lp011) | — | Pool + tensor_arena exceeded 128 KB SRAM1 |
| 2026-05-13 ~23:00 | 124 KB | -1002 | 126,016 | Pre-EON, used most of pool |
| 2026-05-13 ~23:30 | 128 KB (DFSDM moved to SRAM2) | Linker error (Lp011 again because of tensor_arena=2.2 KB in same section) | — | |
| **2026-05-14 ~00:00** | 64 KB (post-EON+int8) | **-1002** | **62,976** | EON+int8 cut MFCC scratch from 110 KB to 63 KB peak (~43% reduction) |
| **2026-05-14 ~00:03** | **96 KB** (current) | **-1002** | **NOT MEASURED — need Live Watch read** | Current state. May need >96 KB or there may be a different failure mode |

---

## Memory layout (current, from `.map` at commit `0dbf89b`)

| Buffer | Address | Size | Region | Purpose |
|---|---|---|---|---|
| `s_ei_pool` | `0x30000000` | 96 KB (`0x18000`) | D2 SRAM1 | EI bump allocator backing store |
| `tensor_arena` | `0x30018000` | 2.2 KB (`0x880`) | D2 SRAM1 | TFLite Micro NN scratchpad |
| (free) | `0x30018880`–`0x3001FFFF` | ~30 KB | D2 SRAM1 | Safety margin |
| `g_AudioBuf` | `0x30020000` | 96 KB | D2 SRAM2 | PCM accumulator for 3-sec recording |
| `s_idleStack` | `0x30038000` | 14 KB | D2 SRAM2 | FreeRTOS idle task stack (bumped from 512 B for TFLite inference room) |
| `s_DfsdmBuf` | `0x3003B800` | 512 B | D2 SRAM2 | DFSDM DMA destination (moved from SRAM1 to free pool space) |
| `s_ei_pool_used` | `0x2404ED94` | 4 B | AXI SRAM `.bss` | Bump pointer (Live Watch this address) |
| `s_ei_pool_high` | `0x2404ED98` | 4 B | AXI SRAM `.bss` | Peak watermark across runs (Live Watch this address) |

D3 SRAM4 (48 KB free past RTT) is not used by this build.

---

## Root cause hypothesis

1. **MFCC's cumulative allocations exceed pool capacity.** EI's DSP block makes ~50 small allocations per inference (FFT scratch, mel filterbank, DCT temps). Each allocation is rounded up to 32-byte alignment by our `ei_malloc`. Cumulative total varies based on:
   - Selected DSP block (MFCC vs MFE — user toggled this earlier in Studio)
   - Audio sample rate (currently 16 kHz)
   - `frame_length`, `frame_stride`, `FFT length`, `num_filters`, `num_cepstral` (currently Studio defaults)
   - Whether CMSIS-DSP optimized paths are used (EON+int8 export pulled in full CMSIS-DSP+NN libraries)

2. **Studio's "Peak RAM usage" estimate is unreliable for our build.** Studio showed 15 KB at one point; we measured 63 KB after EON+int8 with the actual model. The disconnect is because:
   - Studio's estimate is for the **target chip selected in Studio's UI** (Arduino Portenta), not our STM32H747
   - Studio's number does **not include** our 32-byte alignment padding waste per allocation
   - Studio's number assumes CMSIS-DSP optimized code paths; our `EI_PORTING_IAR=1` path may differ

3. **The model can change between Studio re-exports.** The user retrained the classifier after switching MFCC→MFE→MFCC during yesterday's Studio session. The current EON-exported `tflite_learn_985318_3_compiled.cpp` may correspond to a slightly different model than the one we measured the 63 KB peak on.

---

## What's been tried

| Approach | Result | Notes |
|---|---|---|
| Increase pool size (40 → 128 KB) | Worked up to 124 KB pre-EON, then hit SRAM1 ceiling | Edge-sizing pattern user explicitly called out as bad practice |
| Move tensor_arena out of SRAM1 (`EI_TENSOR_ARENA_LOCATION=.sram2`) | Partial — bypassed via direct source edit, but Lp011 still occurred because user removed define before rebuild | Reverted; arena currently in `.sram1` per default define |
| Move `s_DfsdmBuf` from SRAM1 → SRAM2 | ✓ freed 16.5 KB at start of SRAM1 | Permanent change, committed |
| Switch DSP block to MFE | Required full retrain; user reverted to MFCC | Tried briefly in Studio, abandoned |
| Re-export with EON Compiler + int8 quantization | ✓ Cut MFCC scratch peak from 110 KB → 63 KB (43% reduction) | Major win, but still overflows current 96 KB pool somehow |
| EON Compiler (RAM optimized) | Paywalled — Enterprise Trial required | Free tier limit |
| Reduce MFCC parameters in Studio | Greys out with invalid combinations; user tried briefly | `frame_length` 0.02→0.01 needs matching `frame_stride` reduction |

---

## What hasn't been tried (next session priority)

1. **Read `s_ei_pool_used` value via Live Watch after the 96 KB build's failure** — this is the single most useful data point we're missing. Definitive answer for next bump size.
2. **Bump pool to 120 KB** (max SRAM1 with arena in same region, 2 KB margin) — straightforward if `used` is between 96 and 120 KB.
3. **Move `tensor_arena` to `.sram2` permanently** + bump pool to **128 KB** to fill all of SRAM1 — adds ~2 KB headroom.
4. **Multi-segment allocator** spanning SRAM1 (128 KB) + SRAM4 D3 (48 KB free past RTT) = 176 KB usable. Significantly more complex (allocator needs to handle non-contiguous regions). Last resort if 128 KB pool still insufficient.
5. **In-firmware DSP-allocation logger** — temporarily add `RLOG("[EI-ALLOC] sz=", size)` inside `ei_malloc` to print every allocation request. After one inference, sum the sizes to know the **exact** model RAM requirement (not approximate). Then size pool with margin.
6. **Reduce MFCC parameters in Studio** + retrain — Studio's defaults are conservative; cutting `FFT length` 256→128, `num_filters` 32→24, `frame_length` 0.02→0.015 should shrink scratch by ~40-50%. Requires retrain (~5 min) + re-export.
7. **Switch to 8 kHz sample rate** — halves both audio buffer (96 KB → 48 KB) and MFCC scratch with minimal accuracy loss for keyword spotting. Requires re-export.
8. **Studio EON Tuner** — automated architecture search within free-tier limits. Can find smaller NN that fits target RAM budget.

---

## Code references

| File | Lines | Purpose |
|---|---|---|
| `CM7/Core/Src/wake_word_test.cpp` | 107 | `#define EI_POOL_SIZE (96u * 1024u)` |
| `CM7/Core/Src/wake_word_test.cpp` | 115-117 | `__no_init static uint8_t s_ei_pool[EI_POOL_SIZE]` with `#pragma location = ".sram1"` |
| `CM7/Core/Src/wake_word_test.cpp` | 131-140 | `ei_malloc` override — bump allocator |
| `CM7/Core/Src/wake_word_test.cpp` | 162-205 | `WakeWordTest_OnIdle` — runs `run_classifier` |
| `CM7/Core/Src/voice_recorder.c` | 184-191 | `s_DfsdmBuf` at `0x3003B800` (SRAM2) |
| `CM7/Core/Src/voice_recorder.c` | 466-471 | `WakeWordTest_Trigger(g_AudioBuf, 16000)` call after DFSDM_DONE |
| `Edge Impulse/edge-impulse-sdk/classifier/postprocessing/ei_postprocessing_common.h` | 56 | Local patch: `(void*)` cast for function-pointer comparison (recurring fix after each re-export) |
| `Edge Impulse/tflite-model/tflite_learn_985318_3_compiled.cpp` | ~104 | `tensor_arena` declaration with `DEFINE_SECTION(STRINGIZE_VALUE_OF(EI_TENSOR_ARENA_LOCATION))` |
| `EWARM/stm32h747xx_flash_CM7.icf` | 58-61, 297 | `.sram1`/`.sram2` section definitions and placement |

---

## Workarounds in place (don't remove)

- 96 KB static pool with bump allocator (Hard Rule #1 compliant — no real malloc)
- 14 KB idle task stack override via `vApplicationGetIdleTaskMemory` (TFLite needs much more than default 512 B)
- One-line patch in `ei_postprocessing_common.h:56` for IAR strict C++ function-pointer comparison
- `s_DfsdmBuf` relocated SRAM1 → SRAM2 to free pool space
- `WAKE_WORD_TEST` preprocessor define gates the entire feature so default build is unchanged

---

## Acceptance criteria (when this bug closes)

- [ ] On-device `s_ei_pool_used` < `EI_POOL_SIZE` after each `run_classifier()` invocation
- [ ] Terminal I/O shows `[EI] classify START` followed by `HEY NOA <pct>` and `Noise <pct>` lines
- [ ] `HEY NOA > 60` when user says "Hey Noa" clearly into mic; `< 30` for silence or unrelated speech
- [ ] `[EI] dsp_ms=<N>` and `[EI] inf_ms=<N>` printed with sensible values (~20-40 ms dsp, ~5-20 ms inf)
- [ ] Existing pipeline (LCD, audio record, SD save, UART, GCS) still works alongside classifier
- [ ] No -1002 errors across 10 consecutive button presses

---

## Related bugs

- [BUG-001](BUG-001_PCM_STREAM_75PCT_TIMEOUT.md) — CLOSED (UART jumper wire); unrelated but in same audio pipeline
- [BUG-002](BUG-002_AXI_OVERFLOW_LCD_ENABLE.md) — CLOSED (RTT relocation to D3 SRAM4); demonstrated the same "find unused RAM region" approach we may need here

---

## Memory references (CLAUDE.md memories)

- [feedback_dont_size_to_edge](../memory/feedback_dont_size_to_edge.md) — Rule established during this debugging cycle: never size to exact peak, always ≥20% margin or natural region boundary
- [project_ei_pool_fallback_plan](../memory/project_ei_pool_fallback_plan.md) — Decision: if SRAM1 max insufficient, move DFSDM to SRAM2 to reclaim SRAM1; SDRAM is off-table
- [feedback_iar_make_vs_rebuild](../memory/feedback_iar_make_vs_rebuild.md) — Default to Make (F7) not Rebuild All; preprocessor changes need Rebuild All
