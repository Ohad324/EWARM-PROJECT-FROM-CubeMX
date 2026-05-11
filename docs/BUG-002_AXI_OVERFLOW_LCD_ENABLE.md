# BUG-002 — AXI SRAM Overflow when LCD/video tasks re-enabled

**Filed:** 2026-05-11 (evening)
**Reporter:** session-end attempt to revert `AUDIO_DEBUG_LCD_DISABLED 1` → `0`
**Branch:** `test/inject-flasher-thumbnail-no-wifi` (commit base: `4019b95`)
**Severity:** **BLOCKER** — prevents shipping the LCD-on production build
**Status:** Open — investigation in progress (IAR optimization-level tuning to be tested)
**Related:** BUG-001 (`BUG_PCM_STREAM_75PCT_TIMEOUT.md`, now closed — root cause was disconnected jumper wire)

---

## Symptom

After flipping `AUDIO_DEBUG_LCD_DISABLED` from `1` to `0` in `CM7/Core/Src/main.c:441` (re-enables creation of `TouchGFXTask` + `videoTask` in the RTOS init block at line 443-448), the linker rejects the build:

```
Error[Lp011]: section placement failed
    unable to allocate space for sections/blocks with a total estimated
    minimum size of 0x3f55c bytes (max align 0x20)
    in <[0x24000000-0x2404ffff], [0x24050804-0x2406ffff], [0x24070050-0x2407ffff]>
    (total uncommitted space 0x392ac).
    Uncommitted:
      [0x24000000-0x2404ffff]:   0x9b00
      [0x24050804-0x2406ffff]: 0x1f7fc
      [0x24070050-0x2407ffff]:   0xffb0

Total number of errors: 1
Build failed
```

### Decoded

- **Need:** `0x3F55C` ≈ **259 KB** of `.bss` + `.data` + named-section allocations
- **Have:** `0x392AC` ≈ **234 KB** of uncommitted AXI SRAM space
- **Shortfall:** `0x62B0` ≈ **25 KB**

The three uncommitted fragments are split because of fixed-address placements (`g_irq_log` at `0x24050000`, `ucHeap` block at `0x24050804`, `s_dma_rx_buf` etc. in the `.axi_sram` named region).

---

## Root cause

AXI SRAM is at 97.5 % utilisation in the headless-audio build (per [docs/MEMORY_MAP_AND_SW_BLOCKS.md](MEMORY_MAP_AND_SW_BLOCKS.md) audit, 2026-05-11 morning). Re-enabling TouchGFX + video tasks adds:

- TouchGFX engine state (.bss, working buffers)
- `videoTaskHandle` stack + task state
- TouchGFX widget tree (Screen1, MusicScreen)
- DMA2D-related `.bss`
- Any auxiliary buffers freshly used by the LCD pipeline

…totalling **~25 KB more than AXI can spare**. Without LCD, the 13 KB free margin was already tight; with LCD it's −25 KB underwater.

---

## Reproduction

1. Check out `4019b95` (or latest on `test/inject-flasher-thumbnail-no-wifi`).
2. Edit `CM7/Core/Src/main.c:441`:
   ```c
   #define AUDIO_DEBUG_LCD_DISABLED 0
   ```
3. Build:
   ```
   iarbuild EWARM/STM32H747I-DISCO.ewp -make STM32H747I-DISCO_CM7 -log warnings
   ```
4. Observe linker error `Lp011`.

---

## Mitigation options (ordered by Change Priority — see CLAUDE.md)

### Option A — IAR Compiler Optimization (Score 1, low risk) — **TRIED 2026-05-11, FAILED**

Switched the project's CM7 configuration to **Optimization Level: High, Strategy: Size** through the IAR IDE.

Result: **shortfall grew** from 25 KB to 30 KB.
- Before: need `0x3F55C` (259 KB) — short by 25 KB
- After (size-opt): need `0x40848` (264 KB) — short by 30 KB

Reason: AXI usage is dominated by static `.bss` (framebuffer, heap, queues, HAL handles, task stacks), not by Flash code. Size optimization shrinks the `.text` section in Flash but doesn't touch `.bss`. The aggressive inlining at High/Size can even slightly grow `.bss` (unrolled static tables, larger compiler-introduced scratch buffers).

**Verdict:** Insufficient. Option A alone cannot recover 25 KB of AXI `.bss`. Move to Option B.

### Option B — Move FreeRTOS heap to D2 SRAM1 (Score 1, local, medium risk)

`ucHeap[]` is 96 KB at `0x24050804` in AXI. D2 SRAM1 has 127.5 KB free (only `s_DfsdmBuf` 512 B at the start). Moving the heap to the **end** of SRAM1 (e.g. `0x3000_4400`+ to avoid DFSDM adjacency) frees the largest single block in AXI.

Steps:
1. `FreeRTOSConfig.h`: set `configAPPLICATION_ALLOCATED_HEAP = 1`
2. `main.c`: declare `uint8_t ucHeap[100000] @ ".sram1";` (or with `#pragma location = ".sram1"`)
3. Ensure `.sram1` placement in `.icf` is preserved (already done)

**Risk:** yesterday's debugging session (commit `745de5d`) revealed heap-near-DFSDM corruption. The deliberate offset (`0x30004400`+) avoids that, but verify with the scope.

### Option C — Shrink the partial framebuffer (Score 1, local, medium risk)

`TouchGFX_Framebuffer` (FB_BLOCK) is 281 KB at `0x24000000`. The current PFB strategy uses 4 strips of `800 × 120 × 3 B` = 288 KB. Reducing to 6 strips of `800 × 80 × 3 B` = 192 KB frees **89 KB** but adds DMA2D rendering overhead. Requires TouchGFX `setMaxBlockLines(80)` + `setNumberOfBlocks(6)` changes.

### Option D — RGB565 instead of RGB888 (Score 1, scope creep)

Halves the framebuffer size (141 KB instead of 281 KB). Requires DMA2D format change, Bitmap format change, possibly OTM8009A re-init. Larger blast radius — postpone unless A/B/C all fail.

---

## Recommended path

1. **First**: try Option A (compiler optimization for size — local, no code change, reversible). Quick experiment.
2. If A is insufficient: combine with **Option B** (heap to D2 SRAM1). Together they should comfortably free 25 KB+.
3. Skip C and D unless A+B fail.

Do not bundle B/C/D into the same build attempt — one variable at a time per `feedback_two_failures_rule.md` and tonight's small-step rule.

---

## Critical files

- [CM7/Core/Src/main.c:441](../CM7/Core/Src/main.c#L441) — the `AUDIO_DEBUG_LCD_DISABLED` define
- [CM7/Core/Src/main.c:443-448](../CM7/Core/Src/main.c#L443-L448) — gated task creation block
- [CM7/Core/Inc/FreeRTOSConfig.h:85](../CM7/Core/Inc/FreeRTOSConfig.h#L85) — `configAPPLICATION_ALLOCATED_HEAP` setting
- [EWARM/stm32h747xx_flash_CM7.icf](../EWARM/stm32h747xx_flash_CM7.icf) — section regions (already supports `.sram1` / `.sram2`)
- [EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map](../EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map) — current placement audit (regenerated per build)
- [docs/MEMORY_MAP_AND_SW_BLOCKS.md](MEMORY_MAP_AND_SW_BLOCKS.md) — pre-LCD memory layout reference

---

## Verification (after fix applied)

1. `iarbuild` exits with `0` errors and `0` warnings.
2. `.map` shows total AXI used ≤ 512 KB with positive uncommitted balance.
3. Flash via cspybat — board boots, TouchGFX renders MusicScreen, button press still triggers voice recording.
4. STT pipeline still works end-to-end (REC_NNN.wav → GCS HTTP 200 → transcript returned).
5. RTT log shows no `LTDC_ER (FUIF)` IRQs during steady-state (per CLAUDE.md PFB findings).

---

## Out of scope

- Re-enabling `s_mjpeg_decode_active` (Music_InjectTestThumb path) — separate bug, not gated by this define.
- Wake-word detection enablement — Edge Impulse SDK already vendored but not yet wired into VoiceRecTask.

---

## References

- [CLAUDE.md "Change Priority Rules"](../CLAUDE.md) — score-1 fixes preferred
- [CLAUDE.md PFB section findings 2026-05-06](../CLAUDE.md) — current framebuffer architecture
- [feedback_no_ewp_edits.md](file://C:/Users/Ohad/.claude/projects/c--TouchGFXProjects-MyApplication/memory/feedback_no_ewp_edits.md) — never edit `.ewp` directly; use IDE
- [feedback_two_failures_rule.md](file://C:/Users/Ohad/.claude/projects/c--TouchGFXProjects-MyApplication/memory/feedback_two_failures_rule.md) — single-variable change discipline
