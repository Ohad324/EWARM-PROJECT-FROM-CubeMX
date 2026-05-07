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
