---
name: jlink-debug
description: SEGGER J-Link Commander debugging — generic command reference and scripting patterns. Use when the user wants free-running register/memory reads, scripted dumps, flash programming via .jlink scripts, or any debugging that needs the target to keep running. Complementary to cspy-debug — use J-Link when you don't need symbol resolution or when halting would disturb live peripheral state. Project-specific addresses, peripherals, and recipes belong in CLAUDE.md, not here.
---

# J-Link Commander Debugging — Generic Playbook

You are operating as an expert in **SEGGER J-Link Commander**. This skill is the **tool reference** — command names, syntax, scripting patterns, anti-patterns. Project-specific addresses and recipes live in `CLAUDE.md`.

**Authoritative reference:** the command list returned by `JLink.exe ?`. SEGGER's UM08001 *J-Link/J-Trace User Guide* documents the same set with longer prose. Baseline used here: J-Link Commander V9.34b on Windows.

## Switch tools when source-sync matters

**Heuristic learned on this project:** if the question is "which symbol / function / variable is doing what?", reach for cspybat — it has the .out/ELF loaded so it resolves symbols. J-Link Commander has no symbol table; every address is hand-extracted from the linker `.map`. When you find yourself grepping the `.map` for more than 2-3 symbols to feed into a `.jlink` script, **switch to cspybat instead** — one macro can read N symbols by name in a single session.

J-Link's strength is **free-running, no-halt, no-symbol** sampling: live peripheral state, soak tests, post-flash health checks, "did this address change in the last second?" cspy's strength is **symbol-aware halt-mode probes**: BPs by function name, struct-field reads, cross-build robustness when addresses move. Use the right tool for the question.

## When to reach for J-Link Commander

| Need | Tool |
|---|---|
| Free-running register / memory reads (target keeps executing) | **J-Link** — DAP reads don't halt the CPU |
| Scripted post-flash health check | **J-Link** — fast, single invocation |
| Read live peripheral state that would alias under halt | **J-Link** (no `Halt`) |
| BP at function entry by symbol | cspybat / IDE — J-Link has no symbol table |
| Read C variable / struct field by name | cspybat / IDE — J-Link can't resolve symbols |
| Flash programming from a CLI / script | **J-Link** (`LoadFile` + `Reset` + `Go`) |
| RTT capture without holding a debug session | `JLinkRTTLogger.exe` (sibling tool) |

## Hard rules

1. **One J-Link connection at a time.** Kill held holders (debugger IDEs, prior `JLink.exe`, RTT viewers/loggers, cspybat) before any new run.
2. **Always end the script with `Exit`.** Without it, JLink waits for interactive input and the wrapper hangs.
3. **Always redirect output:** `JLink.exe ... -CommandFile script.jlink > log.txt 2>&1`.
4. **`-autoconnect 1` skips the device-select prompt.** Use it.
5. **No interactive-only commands in scripts.** `?`, `Term`, `SWOView`, `rttview` — they hang in non-interactive mode.
6. **Memory reads do NOT halt the CPU.** `Mem32 ADDR, N` reads through the DAP while the core keeps executing. Killer feature.
7. **`Halt` may disturb peripheral state on some MCUs.** Some peripherals keep running while the CPU halts. If reading their registers at halt returns a constant value across all reads, suspect halt-induced bus aliasing — read free-running instead.
8. **Hardware BP count is limited** (typically 6–8 FPB comparators on Cortex-M). Software BPs work for RAM-resident code; flash-resident code needs an FPB BP.

## Syntax notes

- **Comma after address.** `Mem32 0xADDR, 1` (not space-separated). Same for `Mem`, `Mem8`, `Mem16`, `Mem64`.
- **Write commands `W1`/`W2`/`W4`/`W8`** with comma between addr and data: `W4 0xADDR, 0xDATA`.
- **`Mem32 ADDR, NumItems`** — count of items, not bytes. `Mem8 ADDR, NumBytes` is bytes.
- **`Mem` (no suffix)** prints ASCII alongside hex — useful for strings, useless for register dumps.
- **`Sleep <ms>`** — milliseconds, no unit suffix. CPU keeps running during the sleep.
- **`Exit`** ends the session.
- **`EoE 1` (`ExitOnError 1`)** — recommended at the top of any script.
- **`Connect` is implicit** with `-autoconnect 1`. Otherwise, put `Connect` first.
- **`SetBP <addr> [A/T] [S/H]`** — A=ARM, T=Thumb, S=Software, H=Hardware. Cortex-M is Thumb-only.
- **No symbol resolution.** Look symbols up in the linker `.map` first.
- **Zone prefix `Mem32 [<Zone>:]<Addr>, N`** — for Cortex-M usually omit the zone.

## Canonical command set (verified against `JLink.exe ?`)

### Connection / setup
| Command | Syntax |
|---|---|
| `Connect` | `Connect` |
| `Device` | `Device <DeviceName>` |
| `SelectInterface` | `SelectInterface <Interface>` |
| `Speed` | `Speed <freq\|auto\|adaptive>` |
| `USB` | `USB [<SN>]` |
| `IP` | `IP <IPAddr>` |
| `Power` | `Power <On\|Off> [perm]` |
| `VTREF` | `VTREF <mV>` |

### Memory access (the workhorse)
| Command | Syntax | Use |
|---|---|---|
| `Mem` | `Mem [<Zone>:]<Addr>, <NumBytes>` | bytes + ASCII |
| `Mem8` / `Mem16` / `Mem32` | `Mem<N> [<Zone>:]<Addr>, <NumItems>` | typed reads |
| `W1` / `W2` / `W4` / `W8` | `W<N> [<Zone>:]<Addr>, <Data>` | typed writes |
| `SaveBin` | `SaveBin <filename>, <addr>, <NumBytes>` | dump RAM range to host file |
| `VerifyBin` | `VerifyBin <filename>, <addr>` | compare host file vs target |
| `LoadFile` | `LoadFile <FileName> [, <Addr>]` | program flash |

### CPU / register / execution control (mostly halt-implying)
| Command | Syntax |
|---|---|
| `Halt` / `IsHalted` / `WaitHalt [<TimeoutMs>]` | halt control |
| `Go` / `Reset` / `ResetX <DelayAfterReset>` / `RSetType <Type>` | resume / reset |
| `Step [<NumSteps>]` | single-step (implies halt) |
| `Regs` | dump CPU registers (implies halt) |
| `RReg <RegName>` / `WReg <RegName>, <Value>` | one register |
| `SetPC <Addr>` | set PC |
| `SetBP <addr> [A/T] [S/H]` / `ClearBP <BP_Handle>` | breakpoints |
| `SetWP <Addr> [R/W] [<Data> [<D-Mask>] [A-Mask]]` / `ClearWP <WP_Handle>` | watchpoints |
| `MoE` | mode-of-entry — why CPU halted |
| `VCatch <Value>` | vector catch |

### CoreSight low-level (rare)
`ReadAP <RegIndex>` / `WriteAP <RegIndex>` / `ReadDP <RegIndex>` / `WriteDP <RegIndex>`.

### Flash
| Command | Syntax |
|---|---|
| `LoadFile` | `LoadFile <FileName> [, <Addr>]` |
| `Erase` | `Erase [<SAddr>, <EAddr>]` |
| `VerifyBin` | `VerifyBin <filename>, <addr>` |

### Trace / SWO
`SWOSpeed`, `SWOStart [<Speed>]`, `SWOStop`, `SWOStat`, `SWORead`, `SWOShow`, `SWOFlush`, `SWOView` *(interactive — do not script)*. `STraceStart` / `STraceStop` / `STraceRead` for ETM.

## ITM stimulus port — fast non-blocking trace via SWO

**Reach for ITM when:** investigating "where does the code actually go?" in timing-sensitive code (ISRs, render pipelines, audio paths) where adding `printf` / mutex-guarded logs would mask the bug. One ITM write ≈ 30 ns vs ~5 µs for a queue-backed log.

**ITM is a Cortex-M silicon block at `0xE0000000`** with 32 stimulus ports (1-way mailboxes). Firmware writes to a port; ITM packetizes; data streams out the SWO pin to the J-Link. No new wires, no buffer, no ISR. FIFO-full = silent drop. No probe attached = silent drop.

**Authoritative refs:** ARM DDI 0403E §C1.8 (architecture) · STM32 PM0253 §5 (register map, easier read) · IAR C-SPY Debugging Guide UCSARM-26 (IDE setup). For 90% of project use, **PM0253 §5 + the IAR guide** is the tier-1 stack.

### Target-side firmware setup (one-time at boot)

```c
#define DEMCR    (*(volatile uint32_t*)0xE000EDFC)
#define ITM_LAR  (*(volatile uint32_t*)0xE0000FB0)
#define ITM_TCR  (*(volatile uint32_t*)0xE0000E80)
#define ITM_TER  (*(volatile uint32_t*)0xE0000E00)

DEMCR   |= (1u << 24);          // TRCENA — master enable for DWT/ITM/ETM
ITM_LAR  = 0xC5ACCE55u;         // unlock (CoreSight magic key)
ITM_TCR  = 1u;                  // ITMENA = 1
ITM_TER  = 0xFFFFFFFFu;         // enable all 32 stimulus ports
```

### Target-side write syntax

```c
/* CMSIS helper (port 0 only, blocks if FIFO full) */
ITM_SendChar('X');                                       // requires <core_cm7.h>

/* Raw MMIO (any port, any width) */
*((volatile uint8_t  *)(0xE0000000 + 4*N)) = byte;       // port N, 1 byte
*((volatile uint16_t *)(0xE0000000 + 4*N)) = halfword;   // port N, 2 bytes
*((volatile uint32_t *)(0xE0000000 + 4*N)) = word;       // port N, 4 bytes
```

### Pattern: state-machine markers (the killer use case)

Drop one character at every state transition in the suspect pipeline:

```c
allocateBlock()              { ITM_SendChar('A'); ... }
markBlockReadyForTransfer()  { ITM_SendChar('M'); ... }
tryTransmitBlock()           { ITM_SendChar('T'); ... }
transmitBlock()              { ITM_SendChar('X'); ... }
freeBlockAfterTransfer()     { ITM_SendChar('F'); ... }
EndOfRefreshCallback()       { ITM_SendChar('E'); ... }
```

SWO output `AMTXFE AMTXFE AMTXFE...` = healthy 4-strip pipeline.
Output `AMT AMT AMT...` = transmitBlock never reached → bug.
Visual pattern recognition at human speed; the missing letter is instant.

### Host-side capture (J-Link Commander)

```
SWOStart 2000000     // start at 2 MHz SWO baud — good default for 400 MHz CPU
SWOShow              // pretty-print port 0 (interactive — do not script)
SWORead              // dump raw buffer to console (script-safe)
SWOStat              // overflow / byte counts
SWOStop              // end capture
```

For scripted file capture, use the standalone tool:
```
JLinkSWOViewer.exe -device <DeviceName> -if SWD -speed 4000 -swofreq 2000000
```

### Host-side capture (IAR C-SPY, GUI only)

| Action | Where |
|---|---|
| Configure SWO speed | Project Options → Debugger → J-Link → SWO |
| Select active stimulus ports | Project Options → Debugger → J-Link → ITM Stimulus Ports |
| Live trace window | View → SWO Trace (during debug) |
| Per-port log windows | View → ITM Stimulus (during debug) |
| Redirect `printf` → ITM | Library Configuration / `__write` redirect to `ITM_SendChar` |

No scriptable C-SPY syntax for ITM config — it's GUI-only. cspybat inherits the project settings.

### ITM register map (`0xE0000000` base) — full reference

| Offset | Register | Purpose |
|---|---|---|
| `0x000` | `STIM[0..31]` | Stimulus port write-only (32 ports × 4-byte stride) |
| `0xE00` | `TER` (Trace Enable) | bit N enables stimulus port N |
| `0xE40` | `TPR` (Trace Privilege) | groups of 8 ports → user-mode access |
| `0xE80` | `TCR` (Trace Control) | bit 0 = ITMENA (master enable), TS/sync config |
| `0xFB0` | `LAR` (Lock Access) | Write `0xC5ACCE55` to unlock |
| `0xFB4` | `LSR` (Lock Status) | bit 1 = locked |
| `0xFD0..0xFFC` | ID / PID / CID | CoreSight component identification |

Master enable also requires `DEMCR.TRCENA = 1` at `0xE000EDFC` bit 24.

### Conventions and gotchas

- **Port 0** for character markers (works with `ITM_SendChar`).
- **Port 1** for 32-bit values (cycle counter, bitmask, address).
- Guard with `#ifndef RELEASE_BUILD` if measurable cost in tight loops.
- Keep markers ≤ 1 byte each — they pattern-match better in the SWO stream than multi-byte strings.
- **TPIU clock mismatch on PLL switch.** J-Link logs `Other than PLL1 clock as CPU clock detected` during early boot. Re-issue `SWOStart` after `SystemClock_Config` if SWO output is garbled.
- **`SWO speed = CPU_clk / divisor`**. At 400 MHz CPU, 2 MHz SWO needs divisor = 200.
- **No probe attached → silent drop.** Don't rely on ITM in standalone runs. Use `RLOG` for logs that must persist.
- **SWO bandwidth ceiling ≈ 200 KB/s at 2 MHz.** Fine for character markers; full memory dumps will saturate. Use `SaveBin` for that.
- **Release builds** may have `TRCENA` cleared — set it unconditionally at boot if Release-build trace is desired.
- **FIFO-ready check** before raw-MMIO writes: read `STIM[N]` first; non-zero = ready. CMSIS `ITM_SendChar` does this for you.
- **Packet format on the wire** (DDI 0403E §C1.10) — 1-byte header `[port_id<<3 | size_code]` + 1/2/4 data bytes. Only matters if writing a custom decoder; J-Link handles it.

### Diagnostics
`?` *(interactive)*, `ShowHWStatus`, `ShowFWInfo`, `ShowConf`, `Uptime`, `TestRSpeed [<Addr> [<Size>] [<NumBlocks>]]`, `TestWSpeed`, `TestCSpeed [<RAMAddr>]`.

### Flow control / session
| Command | Syntax |
|---|---|
| `Sleep` | `Sleep <Delay>` (ms) |
| `Log` | `Log <filename>` (tee output) |
| `EoE` | `ExitOnError <1\|0>` |
| `Exit` | `Exit` (always end the script with this) |
| `Reboot` | `Reboot [force] [<Timeout[ms]>]` |

### Most-used commands
`Mem32`, `W4`, `Sleep`, `LoadFile`, `Reset`, `Go`, `Halt`, `Regs`, `SetBP`/`ClearBP`, `Exit`.

## Canonical .jlink script template

```jlink
// my_probe.jlink — purpose: <describe>

EoE 1                           // bail on first error

Mem32 0xADDR1, 1
Mem32 0xADDR2, 1

W4 0xADDR3, 0xCLEAR_MASK        // clear sticky flags if needed

Sleep 1000                      // observation window
Mem32 0xADDR1, 1

Exit
```

## Canonical .bat wrapper

```bat
@echo off
setlocal
set JLINK=C:\Program Files\SEGGER\JLink_V<ver>\JLink.exe
set SCRIPT=path\to\my_probe.jlink
set LOG=path\to\runs\my_probe.log

taskkill /F /IM JLink.exe          2>nul
taskkill /F /IM JLinkRTTViewer.exe 2>nul
taskkill /F /IM JLinkRTTLogger.exe 2>nul

if not exist "path\to\runs" mkdir "path\to\runs"

"%JLINK%" -device <DeviceName> -if SWD -speed 4000 -autoconnect 1 -CommandFile "%SCRIPT%" > "%LOG%" 2>&1

echo === Done === log: %LOG%
type "%LOG%"
endlocal
```

## Generic recipes (substitute project addresses)

### "Has this register changed?"
```jlink
EoE 1
Mem32 0xADDR, 1
Sleep 1000
Mem32 0xADDR, 1
Exit
```

### "Is a sticky-flag firing?"
```jlink
EoE 1
Mem32 0xSTATUS, 1
W4    0xCLEAR, 0xMASK
Sleep 1000
Mem32 0xSTATUS, 1
Exit
```

### "Soak test"
```jlink
EoE 1
Mem32 0xADDR, 1
Sleep 1000
Mem32 0xADDR, 1
Sleep 1000
Mem32 0xADDR, 1
Sleep 1000
Mem32 0xADDR, 1
Exit
```

### "Save memory to disk"
```jlink
SaveBin path\to\dump.bin, 0xSTART, 0xLENGTH
Exit
```

### "Flash + reset + run"
```jlink
EoE 1
LoadFile path\to\firmware.hex
Reset
Go
Exit
```

### "Forensic halt — Cortex-M fault regs"
```jlink
Halt
Regs
MoE
Mem32 0xE000ED28, 1              // SCB.CFSR
Mem32 0xE000ED38, 1              // SCB.BFAR
Mem32 0xE000ED34, 1              // SCB.MMFAR
Exit
```

### "Hardware breakpoint at a flash address"
```jlink
SetBP 0xFLASH_ADDR, T, H         // Thumb, Hardware
Go
WaitHalt 30000
MoE
Regs
ClearBP 1
Go
Exit
```

## Anti-patterns to refuse

- **`-NoGui 1`** when the user wants the GUI / flash progress visible.
- **Forgetting the final `Exit`** — wrapper hangs.
- **Single one-shot snapshots** for tests of live behavior — prefer multi-sample with `Sleep`.
- **`Reset` mid-investigation** — wipes the state you were observing.
- **cspybat-style `&symbol`** in JLink scripts — no symbol table.
- **Embedding interactive-only commands** in non-interactive scripts.
- **Inventing commands** — if it isn't in the canonical list, it doesn't exist.
- **Lowercase aliases** `r` / `g` / `q` / `qc` — GDB / older conventions; canonical names are `Reset` / `Go` / `Exit`.

## Output discipline

- State the addresses, what you expect, and what would falsify before generating the script.
- Summarize what changed vs the prediction; don't paste raw `Mem32` dumps without interpretation.
- If the wrapper times out: missing `Exit`, an interactive command, or a held probe. Check those three first.
- Project-specific addresses live in `CLAUDE.md`. This skill stays generic on purpose.