---
name: cspy-debug
description: Expert IAR cspybat + C-SPY macro debugging on this STM32H747I-DISCO project — use when the user wants to set breakpoints, read registers/memory, probe boot stages, or diagnose peripherals (LTDC/DSI/DMA2D/SAI4/DFSDM/FMC) via macros. Invoke for any task involving .mac files, cspybat invocations, or BP-based diagnostics.
---

# cspybat + C-SPY Macro Debugging — Project Playbook

You are operating as an expert in IAR C-SPY macro debugging on the STM32H747I-DISCO Voice Recorder project. The authoritative reference is `CLAUDE.md` PART 1–6 ("C-SPY Macro and cspybat Syntax Reference") — read it before writing any macro you are unsure about. This skill captures the project-specific patterns layered on top of that reference.

## Switch tools when source-sync matters

**Heuristic learned on this project:** if the question is "which symbol / function / variable is doing what?", reach for **cspybat** — the .out/ELF is loaded, so symbols, struct fields, and source lines resolve automatically. J-Link Commander has no symbol table; every address has to be hand-extracted from the linker `.map`. The moment you find yourself looking up more than 2-3 symbols in the `.map` to feed into a `.jlink` script, **switch to cspybat** — one macro can read N symbols by name in a single session and survives rebuilds when addresses move.

cspy's strength: **symbol-aware halt-mode probes** (BPs by function name, struct-field reads, source-line BPs, cross-build robustness). J-Link's strength: **free-running, no-halt, no-symbol** sampling (live peripheral state, soak tests, post-flash health checks). Use the right tool for the question.

If a `.jlink` probe returns mysterious garbage and the addresses came from an old `.map`, the build moved them — convert to a cspybat macro using `&symbol_name` and the problem disappears.

## Decide first: free-running vs halt-mode

| Need | Tool | Why |
|---|---|---|
| BP at function entry by name / source line / offset | **cspybat + C-SPY macro** | Symbol resolution via .out/ELF |
| Read C variable / struct field by name | cspybat (`__readMemory32(&sym, "Memory")`) | Debugger resolves addresses |
| Free-running mem32 sample (target keeps running) | **J-Link Commander** (`boot_pipeline.bat`) | No halt → no LTDC alias-on-halt issue |
| Multi-stage boot timeline | **`boot_pipeline_multi.bat`** (cspybat orchestrator) | Many checkpoints × lifecycle hooks |
| LCD live state post-LTDC-init | J-Link Commander free-running OR read C-side struct (`hltdc.LayerCfg[0].FBStartAdress`) | LTDC registers alias to GCR (0xC0002220) at halt — DBGMCU has no LTDC freeze bit on H7 |

## Hard rules (non-negotiable)

1. **One useful BP hit per cspybat session** — passive `__setCodeBreak` reliably fires only the FIRST BP per invocation. For N checkpoints, run cspybat N times via an orchestrator (`*.ps1` + `*_one_bp.mac.tpl` template + `@@CHECKPOINT@@` / `@@SKIP@@` substitution). The exception is `__hwRunToBreakpoint` (active advance) which can chain multiple checkpoints in one session.
2. **Stop-and-fix at the first failing BP** — when an orchestrator runs N checkpoints and one fails (timeout / lockup), all downstream BPs are gated on the same broken state. STOP, read the per-iteration log, fix firmware, rebuild, re-run from N. Don't plough through.
3. **One macro file, overwrite per run** — when the user asks for sequential BP probes, generate ONE `.mac` file per iteration by templating; don't sprawl files.
4. **No `printf`, no `__go`/`__stop`/`__halt`/`__break`/`#define`/`#include`** — they don't exist. Use `__message` and the lifecycle hooks (`execUserSetup`, `execUserExit`).
5. **Width-with-leading-zero quirk** — `%02d` is parsed as octal and fails. Use `%d` or `%2d`. For hex padding `%08X` is fine.
6. **SDRAM (0xD0000000) is unreadable before `MX_FMC_Init`** — reading it pre-FMC bus-faults the DAP and aborts the session with `Operation error`. Only sample SDRAM at post-FMC checkpoints.
7. **LTDC layer regs alias at halt** — when halted post-`MX_LTDC_Init`, all 11 LTDC regs return `0xC0002220` (the GCR). Read the `hltdc` struct in DTCM/AXI instead, or use J-Link free-running.
8. **Release the probe between sessions** — only one J-Link connection at a time. Before reconnecting:
   ```
   taskkill /F /IM IarIdePm.exe
   taskkill /F /IM JLink.exe
   taskkill /F /IM JLinkRTTViewer.exe
   taskkill /F /IM CSpyBat.exe
   ```
9. **Always redirect both streams** — `> log.txt 2>&1`. `__message` goes to stdout; cspybat errors to stderr.
10. **Always read the log** — "Build/flash succeeded" is not proof the code ran. Look for `===== SESSION START =====` AND `===== SESSION END =====`. Missing END = macro crashed mid-run.

## Canonical project assets

- **Build**: `"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\iarbuild.exe" EWARM\STM32H747I-DISCO.ewp -make STM32H747I-DISCO_CM7 -log warnings`
- **Flash without debug**: `EWARM\auto_build_flash.bat` (or `cspybat ... --download_only` for a flash-only run)
- **Free-running probe**: `EWARM\boot_pipeline.bat` (J-Link Commander mem32)
- **Halt-mode multi-checkpoint**: `EWARM\boot_pipeline_multi.bat` / `boot_pipeline_lcd.bat` / `verify_pfb.bat`
- **Templates**: `EWARM/*_one_bp.mac.tpl` with `@@CHECKPOINT@@` and `@@SKIP@@` placeholders
- **Orchestrators**: `EWARM/*.ps1` — generate per-iteration `.mac`, run cspybat with watchdog timeout, save logs to `EWARM/runs/<probe>/<NN_tag>.log`
- **Linker map** (for symbol → address sanity): `EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map`

## Canonical macro template

```c
// my_probe.mac
bp = 0;

execUserSetup()
{
    __message "===== SESSION START =====\n";
    bp = __setCodeBreak("MX_LTDC_Init", 0, "1", "TRUE", "onHit()");
    if (bp == 0) {
        __message "[ERROR] BP install failed\n";
    } else {
        __message "[OK] BP installed h=", bp, "\n";
    }
}

onHit()
{
    __message "[BP-HIT] MX_LTDC_Init\n";
    __message "  cyc=0x", __readMemory32(0xE0001004, "Memory"):%08X, "\n";
    /* C-side struct read works at halt (LTDC regs would alias) */
    __message "  FB=0x", __readMemory32(&hltdc.LayerCfg[0].FBStartAdress, "Memory"):%08X, "\n";
}

execUserExit()
{
    __clearBreak(bp);
    __message "===== SESSION END =====\n";
}
```

## Symbol resolution cheatsheet

```c
val   = g_state;                              /* value of C symbol */
addr  = &g_state;                             /* address of C symbol */
__readMemory32(&hltdc.Init.HorizontalSync, "Memory");  /* struct field */
__hwRunToBreakpoint(&main, 5000);             /* address arg must be int — use & */
__evaluate("g_state",  &result);              /* dynamic / string-based */
defined = __isMacroSymbolDefined("name");
```
`__symbolAddress("name")` does NOT exist in IAR EW v9.4.6 — use `&` directly.

## Memory read / write

```c
val = __readMemory32(0x58024400, "Memory");    /* RCC->CR */
__writeMemory32(0x00000001, 0xE000ED88, "Memory");  /* note: value first, then address */
```
Sizes: 8 / 16 / 32 / 64. Zone is almost always `"Memory"`.

## Project-specific debug recipes

### Boot stage verification (clocks, MPU, peripheral inits, task dispatch)
Use `boot_pipeline_multi.bat` — 25 checkpoints × 3 lifecycle hooks via cspybat. Per-iteration logs in `EWARM/runs/`.

### "Is FUIF firing right now?" (steady-state LCD health)
Use `boot_pipeline.bat` — JLink free-running mem32. Read the IRQ ring buffer at `0x24008000` (256 entries × 8 B = irq_num + cycle) and `g_irq_idx` at `0x24008800` (since the AXI move, double-check current addresses in `stm32h7xx_it.c` against the plan in `gleaming-dreaming-hartmanis.md` — may have moved to `0x24050000` / `0x24050800`).

### LCD pipeline probe (LTDC / DSI / DMA2D / EOR)
Use `boot_pipeline_lcd.bat` (or `verify_pfb.bat` for the PFB-specific 15 checkpoints). Set BPs at:
- `MX_LTDC_Init` final line — verify `hltdc.LayerCfg[0]` config in RAM
- `HAL_DSI_Refresh` entry — capture DSI/DMA2D state pre-refresh
- `HAL_DSI_EndOfRefreshCallback` entry — LEFT/RIGHT split state
- `LTDC_ER_IRQHandler` entry — fires on FUIF/TERRIF; capture DMA2D state at fault

### Single-shot snapshot at one symbol
`boot_pipeline_cspy.bat` — fastest path for one BP.

## Heap / dynamic-allocation diagnostics — "the silent killer"

> **IRON RULE (Ohad, 2026-05-17):** Every `malloc` MUST be paired with a `free` on every code path that returns. **Unmatched malloc = CPU corruption.** No exceptions, no "I'll free it later", no leaving it to scope exit. If you can't guarantee the matching free on every path (early returns, error branches, exception paths), DO NOT use malloc — use a static buffer instead.

In embedded (especially this STM32H7 project running for hours/days without reset), `malloc` without matching `free` is the worst class of bug. Hard Rule #1 in CLAUDE.md bans it project-wide, but vendored code (Edge Impulse SDK, TouchGFX framework, ST HAL) sometimes ships with `malloc/free` patterns we have to intercept. Reference this section the moment you see `*alloc` anywhere in the call graph.

### Three failure modes to recognize on sight

1. **Out-of-memory crash (the obvious one).** Each unmatched `malloc` locks another chunk of the heap. With no OS to clean up (firmware never exits), the heap fills monotonically. Next `malloc` returns `NULL`. If the caller doesn't check (and most embedded code doesn't), the next dereference is a NULL write → **HardFault**. On Cortex-M7 the fault handler sees `SP=0x1`, `BFAR=0x0`, and the original PC is sometimes lost in a stack-corruption double-fault.

2. **Heap fragmentation (the sneaky one).** Even with freed space, the heap looks like Swiss cheese — many small free regions, none large enough for a single contiguous big request. **You can have 50 KB free in total but `ei_malloc(10*1024)` still returns NULL** because no single hole is ≥ 10 KB. This is what bit us on 2026-05-17 with the EI MFCC pool: the LIFO `ei_free` reclaimed most chunks but out-of-order frees left holes; total used was only 17 KB but the killer alloc still failed for a few iterations until we added headers + LIFO bump-back.

3. **The "works in lab, dies at customer" pattern.** Heap leaks compound at fractions of a KB per inference cycle. Lab tests run for minutes — heap fills minimally, system passes. Customer leaves it running for 8 hours — heap fills, system crashes Thursday afternoon. **There will be no compile-time warning, no log line at boot, no easy reproduction.** This is why CLAUDE.md Hard Rule #1 is absolute.

### Diagnostic recipe when you SUSPECT a heap leak

1. **Static counter on every allocator path.** Add a `volatile uint32_t s_<pool>_used` that increments on alloc and decrements on free. Read via IAR Live Watch — if it monotonically grows across inferences/frames/iterations, you have a leak. (We do this for `s_ei_pool_used` in [CM7/Core/Src/wake_word_test.cpp](CM7/Core/Src/wake_word_test.cpp).)
2. **ITM stream per allocation event.** Use `ITM_EVENT32(<port>, <size>)` on alloc, free, and overflow paths. View in IAR SWO Trace stimulus ports. Gives you a cycle-accurate event log of every heap touch.
3. **BP at the overflow branch.** The instant the allocator returns NULL, you want to halt and inspect: current pool usage, requested size, call stack. Place the BP at the actual `return nullptr` line (or an ITM_EVENT32 marker on that branch — compiler can fold bare `return` into shared epilogue and break our BP).
4. **Per-allocation header for forensics.** Add a small (4–8 byte) header before each user pointer that records the aligned size. Lets `ei_free` do LIFO bump-back AND lets you walk the pool offline to enumerate live allocations.

### Three prevention rules (project-wide)

1. **Every `malloc` MUST have a matching `free` on the same logical path** — including all early-return / error-handling branches. Use `goto cleanup;` patterns rather than scattered returns.
2. **Prefer static allocation.** A `static uint8_t buf[N]` in `.bss` is heap-independent, address-stable across builds, and visible in the `.map` for sizing audits. CLAUDE.md Hard Rule #1 makes this mandatory project-wide.
3. **Use SEGGER SystemView's Heap Monitor** when a leak proves hard to find by code review. It shows graphically which function allocated and never freed. (SystemView is downloaded under [EWARM/SystemView/](EWARM/SystemView/) but not currently wired into the firmware — would need to be integrated if we go this route.)

### Project-specific landmines

- **`xQueueCreate` / `xSemaphoreCreate*` / `xTaskCreate` are heap-backed** even though they look like one-time init calls. Hard Rule #1 mandates the `*Static` variants. The 2026-05-11 silent UART failure documented in CLAUDE.md was caused by `xQueueCreate` putting `xRawBleQueue` at an address that moved when the heap moved.
- **Edge Impulse SDK uses `ei_malloc`/`ei_free` extensively in MFCC.** We override both with a static-pool bump allocator in [wake_word_test.cpp](CM7/Core/Src/wake_word_test.cpp). The override has its own pitfalls (LIFO-only reclaim, 32-byte header overhead per allocation) — see the comment block there for the full story.
- **TouchGFX `OSWrappers::xMessageQueueNew(NULL)` falls back to dynamic** when `NULL` attributes are passed. Always pass a `osMessageQueueAttr_t` with static `cb_mem` and `mq_mem`. The 2026-05-07 deadlock at first VSync was this exact bug.

## Anti-patterns to refuse

- Multi-`.mac`-file sprawl when user wants sequential probes — use one templated file overwritten per iteration
- Continuing the orchestrator past the first BP failure — read the log, fix the cause
- Reading LTDC layer registers post-init while halted (will alias) — read the `hltdc` struct or go free-running
- Touching PLL3 to "fix" anything (see `project_pll3r_change_bricks_app.md`)
- Calling `__setCodeBreak` for many BPs in one session expecting all to fire — only the first reliably does

## Output discipline

- Always state the BP target, the read targets, and the exit criterion before generating the macro.
- After running, summarize: which BP fired, cycle counter, any anomalies vs expected — not a wall of raw log.
- If a BP times out: report the timeout, propose ONE root-cause hypothesis, ask for go-ahead before re-running.
