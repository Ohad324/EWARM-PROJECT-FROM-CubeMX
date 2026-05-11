# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

---

# STM32H747I-DISCO Voice Recorder

## ST Documentation — Design Reference Table

When designing or reviewing changes that affect LTDC, framebuffer placement, FreeRTOS heap, or graphics pipeline, cite these ST documents (all available locally in `docs/`):

| Document | Focus Area | Why It Supports the Current Design |
|---|---|---|
| **AN4861** | LTDC Peripheral | Justifies moving `frameBuf` to AXI SRAM to stop FUIF (FIFO Underrun). Bus isolation = LTDC on AXI, aggressors on FMC. |
| **AN4891** | STM32H7 System Topology | Validates relocating the FreeRTOS heap to SRAM1 (D2 internal). Heap on D2, framebuffer on D1, kernel globals optionally on DTCM. |
| **AN5215** | Bus Bandwidth & Performance | Forensically supports the "Aggressor" behavior of CPU/Cache during contention; explains FMC arbitration and burst latencies. |
| **AN5056** | TouchGFX Graphics Framework | Explicitly supports the **Partial Framebuffer (PFB)** strip strategy used here — `setFrameRefreshStrategy(REFRESH_STRATEGY_PARTIAL_FRAMEBUFFER)` + per-strip `transmitBlock()` callback. |
| **AN5405** | Cache & Coherency | Recommends framebuffer in MPU non-cacheable, shareable region (avoids `SCB_CleanDCache_by_Addr` overhead and races). |
| **RM0399** | Reference Manual (chapters 32–33, 60) | Authoritative for LTDC register layout (§33), DMA2D (§32), DBGMCU freeze bits (§60.5 — confirms no LTDC freeze, hence the LTDC alias-on-halt finding). |

Always read the relevant document before proposing or reviewing a change in these areas — guessing wastes hours, ST docs save them.

## Project Identity
- Board: STM32H747I-DISCO
- Active core: Cortex-M7 (CM4 holds a stop-mode stub at `0x08100000` only)
- Toolchain: CubeMX + IAR EW 9.70.2 (cspybat 9.4.6.1706)
- IAR project: `EWARM/STM32H747I-DISCO.ewp`
- Active target: `STM32H747I-DISCO_CM7`
- IAR tool root: `C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\` (`iarbuild.exe`, `cspybat.exe`)
- Repo layout: `CM4/Core` (CM4 stub), `CM7/Core` + `CM7/TouchGFX` (active app), `EWARM/` (IAR project + probes), `Drivers/`, `Common/`, `Middlewares/`, `docs/` (ST manuals).
- CubeMX source-of-truth: `STM32H747I-DISCO.ioc` (also supports STM32CubeIDE / MDK-ARM, but EWARM is the active toolchain in this project).
- Git: `.git` lives inside the project folder. Remotes: `origin` is a bare repo on OneDrive (`C:/Users/Ohad/OneDrive - sightsys/STM32H747-SCREEN/MyApplication.git`) for local backup, `github` is a GitHub mirror.

## Common Commands

All commands are run from the project root `C:\TouchGFXProjects\MyApplication`. Bash and PowerShell both work; the orchestrator scripts (`*.bat`, `*.ps1`) are Windows-native.

**Build (CM7, current active configuration — usually Debug):**
```
"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\iarbuild.exe" EWARM\STM32H747I-DISCO.ewp -make STM32H747I-DISCO_CM7 -log warnings
```
Use `-build` instead of `-make` to force a full rebuild (incremental otherwise). Output: `EWARM/STM32H747I-DISCO_CM7/Exe/STM32H747I-DISCO_CM7.out` and `.hex`. Linker map: `EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map`.

**Build + flash + reset (one-shot):**
```
EWARM\auto_build_flash.bat
```
Kills any held probes, builds, flashes via J-Link, then runs cspybat `--download_only` to release the chip into a running state.

**Single-BP cspybat probe — diagnostic, captures register/RAM dump at one symbol entry:**
```
EWARM\boot_pipeline_lcd.bat            # 9 LCD-pipeline checkpoints (existing)
EWARM\verify_pfb.bat                   # 15 PFB-verification checkpoints (this work)
```
Each `*.bat` calls a `*.ps1` orchestrator that runs cspybat once per BP via `*_one_bp.mac.tpl` template substitution. Per-iteration logs land under `EWARM/runs/<probe_name>/<NN_tag>.log`; combined log at `EWARM/runs/<probe_name>.log`. The "one useful BP hit per cspybat session" rule (see Project-discovered specifics) is why this is one BP per run, not many BPs in one session.

**Free-running J-Link mem32 probe (target keeps running, no halt):**
```
EWARM\boot_pipeline.bat
```
Uses J-Link Commander for live memory reads — useful when LTDC layer registers need to be read post-init (they alias to GCR at halt).

**Release the J-Link probe before reconnecting:**
```
taskkill /F /IM IarIdePm.exe
taskkill /F /IM JLink.exe
taskkill /F /IM JLinkRTTViewer.exe
taskkill /F /IM CSpyBat.exe
```
Required between IAR IDE debug sessions and command-line cspybat runs (J-Link allows only one connection at a time).

**Recovery from a chip lockup (HardFault leaves M7 with `SP=0x1`, J-Link can't attach):**
1. Unplug the J-Link USB cable from CN2, wait 3 seconds, replug.
2. Or press the black RESET button on the STM32H747I-DISCO board.
3. Then re-run cspybat — the freshly-reset chip allows download of new firmware.

**View the .map symbol locations after a build (sanity-check linker placement):**
```
grep "frameBuf\|ucHeap\|g_dbg\|g_irq_log" EWARM\STM32H747I-DISCO_CM7\List\STM32H747I-DISCO_CM7.map
```

## Documentation Rule — MANDATORY, NO EXCEPTIONS

Before proposing or implementing ANY fix (trivial or not), you MUST read all 3 of these sources in order:

1. **User Manual / Application Note** — ST UM or AN relevant to the peripheral (e.g. UM2411, AN5027)
2. **Component Datasheet** — the exact part number on the board (e.g. MP34DT05-A, not a generic)
3. **Community** — ST Community forum, or a colleague/Gemini cross-check for known issues

**Do not write a single line of code until all 3 have been consulted.**

Reason: ST has thousands of developers, excellent docs, and an active community. The answer almost always exists. Guessing wastes hours. Reading saves them.

- Always check `docs/` folder first before asking the user to find a document.
- If a document is missing from `docs/`, tell the user which specific document is needed and why.

## Auto-Approve & Order of Operations

### Auto-Approve Definition

- **Permission:** Claude is authorized to execute Bash commands (terminal commands) without manual confirmation.
- **Scope:** This permission is granted specifically for tool execution and workflow automation.

### Standing Authorization (Ohad, 2026-05-07)

> "I grant you full authority to execute PowerShell and Bash commands automatically to fulfill my requests. Do not ask for confirmation before running diagnostic scripts, file operations, or build commands. If a command requires specific environment variables or elevated permissions, attempt to resolve them or inform me of the specific requirement. Act as an autonomous system engineer; prioritize execution and reporting results over seeking permission."

**What this means in practice:**

- Run `iarbuild.exe`, `cspybat.exe`, `JLink.exe`, `git`, `taskkill`, PowerShell file ops without asking — including the mandatory `taskkill` sequence before any probe attach (see Order-of-Operations Rules below).
- Run image / file conversions (PowerShell `System.Drawing`, `Get-Content`, `Set-Content`, etc.) without asking.
- Do NOT ask for confirmation before invoking diagnostic scripts (`boot_pipeline_*.bat`, `verify_pfb.bat`, `pfb_dispatch.bat`, ad-hoc `.jlink` probes).
- If a command needs an env var or elevated permission, **try to resolve it first**, then report the specific requirement only if it can't be auto-resolved.
- This authorization does **NOT** override the "No-Change" Policy below (READ-ONLY artifacts: architecture, 3rd-party code, CMSIS, ICF, priorities, pinouts) or the destructive-action confirmations in the system prompt (force-push, branch deletion, etc.).
- Reporting style: prefer "executed X, result Y" over "should I run X?". State results, not intentions.

### Operational Order-of-Operations Rules

- **Flexible Order:** Claude may change the order of terminal commands (e.g., performing a `git grep` before a `taskkill`, or reordering build stages) to optimize the debug loop.
- **Mandatory Sequence:** Any command involving `iarbuild.exe` or `cspybat.exe` (and any J-Link probe attach) **MUST** be immediately preceded by the `taskkill` sequence to prevent probe contention:
  ```
  taskkill /F /IM IarIdePm.exe
  taskkill /F /IM JLink.exe
  taskkill /F /IM JLinkRTTViewer.exe
  taskkill /F /IM CSpyBat.exe
  ```

### The "No-Change" Policy — Strict Enforcement

Despite the ability to change command order, these project artifacts are **READ-ONLY**:

- **Architecture:** The 3-task RTOS model is locked (TouchGFXTask, jpegTask, videoTask).
- **3rd-Party Code:** ST Drivers, **CMSIS files**, and TouchGFX library code must not be modified. (Generated TouchGFX glue files like `OSWrappers.cpp` may be hand-modified per Hard Rule #9, but the framework `.lib`, ST Driver `.c`, and CMSIS headers/sources are untouchable.)
- **ICF / Priorities:** Linker files and task priorities are final.
- **Pinouts:** Hardware pin assignments are fixed.

If a fix would require touching anything in this list, **stop and propose the change** with reasoning before editing.

## Main Goal
Preserve the existing working system and make the smallest safe fix possible.

This project is fragile. Prefer surgical fixes over refactors.
Do not change unrelated modules.
Do not break the working thumbnail / music pipeline while working on voice recording.

---

## Change Priority Rules

Before proposing a fix, score the possible paths and prefer the lowest-score option.

| Score | Scope | Guidance |
|------|------|----------|
| 1 | STM32 app code inside project | Preferred |
| 5 | Project-adjacent config, scripts, tool settings | Use only if needed |
| 7 | NORA / ESP32 app code | Avoid unless STM32-side fix is not possible |
| 8 | Infrastructure / boot / partitions / linker / menuconfig | Last resort |

### Rules
- Always consider at least 2 possible fixes for non-trivial issues.
- Prefer score 1 over score 5/7/8 whenever possible.
- If choosing score 5+, explain why a lower-score fix is not sufficient.
- Do not refactor broadly when a local fix is enough.

---

## Working Style

Before changing code:
1. Read the relevant files first.
2. Trace the actual failure path.
3. Explain the likely root cause.
4. Propose the smallest safe fix.

For small score-1 fixes, you may proceed after explaining the reasoning.
For larger or riskier changes, present the plan first.

Do not diagnose from memory alone.
Do not assume an old bug is the same as the current one without checking.

When a change is risky or behavior is mysterious, reach for cspybat + C-SPY macros — set a BP at the suspect site, log the register/RAM state, confirm the prediction. Don't over-use it: skip for small score-1 fixes where flash + visual check is faster. The cspybat syntax reference is in this file (PART 1–6) and the project has working templates (`boot_pipeline_*.bat`, `boot_pipeline_*.mac.tpl`). "Build succeeded" is not proof the code runs as intended.

---

## Hard Technical Rules

These are project rules, not suggestions:

1. Never use `vTaskDelay()` as the 5-second recording mechanism.  
   Use the active DMA drain loop.

2. `g_State` must be `volatile`.  
   Use `__DSB()` after writes to shared state where this project already relies on it.

3. Freeze `g_SampleCount` into a local variable at the start of `SDWriteTask`.

4. DMA ISR callbacks must only signal / release semaphores.  
   No memcpy, no heavy work in ISR.

5. Never increment `g_FileIndex` if `f_open()` failed.

6. Always check `g_SysMode` before touching the SD card in any task.

7. `RTTLogTask` priority must remain `1`.

8. `xLogQueue` send timeout must be `pdMS_TO_TICKS(100)`.

9. **Hard Rule #1: Absolute Ban on Dynamic Allocation.**

   **Prohibited:** `malloc()`, `calloc()`, `realloc()`, `free()`, `pvPortMalloc()`, `vPortFree()`, C++ `new` / `delete`.

   **Prohibited:** Standard RTOS "Create" functions that call `pvPortMalloc` internally: `xTaskCreate`, `xQueueCreate`, `xSemaphoreCreateMutex`, `xSemaphoreCreateBinary`, `xSemaphoreCreateCounting`, `xTimerCreate`, `xEventGroupCreate`, `xStreamBufferCreate`, `xMessageBufferCreate`.

   **Mandatory:** Use the Static equivalents only — `xTaskCreateStatic`, `xQueueCreateStatic`, `xSemaphoreCreateMutexStatic`, `xSemaphoreCreateBinaryStatic`, `xSemaphoreCreateCountingStatic`, `xTimerCreateStatic`, `xEventGroupCreateStatic`, `xStreamBufferCreateStatic`, `xMessageBufferCreateStatic`. Each takes pre-allocated storage (`StaticQueue_t`/`StaticSemaphore_t`/`StaticTask_t`/`StaticTimer_t`/`StaticEventGroup_t` + a typed byte buffer for queue/stream items). `FreeRTOSConfig.h` must have `configSUPPORT_STATIC_ALLOCATION = 1`.

   **No exceptions.** If a dynamic creator is found in legacy code, migrate it to its static variant — do NOT just "document why" and move on. Reason: heap-location bugs are non-local and silent. A queue created via `xQueueCreate` lives wherever `pvPortMalloc` puts it, which depends on heap placement, fragmentation, and prior allocations. Moving the heap (AXI ↔ D2 SRAM1 ↔ DTCM) can silently move the queue into a region with different cache/DMA coherency, breaking ISR↔task data paths without any compile-time signal. Static storage lives at a fixed BSS address forever — heap-independent, predictable, debuggable.

   **2026-05-11 incident:** UART RX silently stopped delivering NORA's `AUDIO:READY` to STM32 after the FreeRTOS heap was moved from D2 SRAM1 back to AXI SRAM (commit `745de5d`). The `xRawBleQueue` in `BLE_UART_Init` was created with `xQueueCreate` (dynamic) and ended up at a different address with different attributes. STM32→GCS pipeline timed out for 10 s; full session lost to debugging. Fix: migrate to `xQueueCreateStatic` + `xSemaphoreCreateMutexStatic`. **Treat any remaining `xQueueCreate`/`xSemaphoreCreate*` call as a latent bug, not a future cleanup.**

   **Hard Rule #3: Section Attribution — Mandatory for static buffers outside default `.data`/`.bss`.**

   When declaring a static buffer that must live in a specific memory region (because of MPU attributes, DMA reachability, cache policy, or domain isolation), Claude MUST explicitly attach a section attribute and state in the comment which region was chosen and why. Buffers that "just work" in default `.bss` (AXI SRAM in this project) do NOT need an attribute — but anything DMA-touched, cache-sensitive, or domain-pinned MUST be tagged.

   **Syntax — IAR EWARM canonical form (prefer this):**
   ```c
   /* Named section — selects a region defined in stm32h747xx_flash_CM7.icf
    * via `place in <region> { section <name> };`. This is the IAR idiom. */
   #pragma location = ".sram2"
   static StaticQueue_t s_queueCB;

   /* Alternative IAR syntax with @ operator — equivalent to #pragma location: */
   static uint8_t buf[N] @ ".sram2";

   /* Literal absolute address — bypasses section regions entirely.
    * Use only when a peripheral / DMA constraint demands a specific address. */
   #pragma location = 0x30000000
   static __no_init uint8_t dfsdm_buf[N];
   ```

   **Syntax — GCC-style attribute (also works on IAR as compatibility extension):**
   ```c
   __attribute__((section(".sram1"))) static uint8_t dma_buf[N];
   __attribute__((section(".axi_sram"))) static uint8_t cpu_buf[N];
   ```
   Use `#pragma location` for new code; the GCC attribute is acceptable when porting from another toolchain or matching surrounding style.

   **Section ↔ memory map (this project, see `EWARM/stm32h747xx_flash_CM7.icf`):**
   | Section | Domain | Base | Size | DMA reach | Cache | Typical use |
   |---|---|---|---|---|---|---|
   | `.axi_sram` (default `.bss`/`.data`) | D1 | `0x24000000` | 512 KB | DMA1/DMA2/MDMA | Cacheable WB | CPU buffers, framebuffer, FreeRTOS heap |
   | `.sram1` | D2 | `0x30000000` | 128 KB | DMA1/DMA2 | MPU Non-Cacheable+Shareable | DFSDM `s_DfsdmBuf`, audio PCM |
   | `.sram2` | D2 | `0x30020000` | 128 KB | DMA1/DMA2 | MPU Non-Cacheable+Shareable | secondary audio, RTOS objects |
   | `.dma_buf` (alias for D2) | D2 | `0x30040000` | 32 KB | DMA1/DMA2 | MPU Non-Cacheable+Shareable | UART DMA RX (`s_dma_rx_buf`) |
   | (BDMA region) | D3 | `0x38000000` | 64 KB | BDMA only | MPU Non-Cacheable | SAI4 PDM (when used) |
   | `.dtcm` | TCM | `0x20000000` | 128 KB | CPU only | Tightly coupled | kernel-critical, NEVER DMA |

   **Comment requirement.** Every section-tagged buffer must have a one-line comment naming the domain and the reason. Examples in this codebase: `s_DfsdmBuf @ 0x30000000` (D3-style D2 SRAM1 for DFSDM DMA), `s_dma_rx_buf @ 0x2407CB00` (`.dma_buf`, UART8 RX DMA target). When in doubt about reachability, consult RM0399 §2 (memory map) + AN4891 (system architecture) before guessing.

   **Why this matters.** STM32H7's bus matrix means a buffer that compiles fine can be silently unreachable by its DMA master. BDMA cannot reach AXI; DMA1/2 cannot reach D3 SRAM; cache lines can deliver stale data to non-coherent DMA. The section attribute is the only compile-time anchor that ties the buffer to a region — without it, a future refactor (e.g. moving the heap, adjusting MPU) can silently relocate the buffer and break the pipeline with no diagnostic signal. Pair this rule with Rule #1 (static allocation): together they make memory placement a property of the source code, not of runtime heap state.

10. Do not call `Error_Handler()` for expected runtime conditions such as missing media or transient peripheral failures.  
    Log and return safely instead.

---

## 480 MHz Clock Rules — Non-Negotiable

The CPU runs at 400 MHz (VOS1, PLL1: PLLM=2, PLLN=64, PLLP=2). These rules prevent silent resets, bus corruption, and peripheral failures.

### 1. Power & Thermal Management

- **Supply Config:** `HAL_PWREx_ConfigSupply()` must be called first to match board hardware (SMPS vs LDO).
- **Voltage Scaling:** Running at VOS1 (sufficient for 400 MHz, safer and cooler than VOS0). You must poll for `PWR_FLAG_VOSRDY` after setting VOS before jumping to 400 MHz.
- **Overdrive Mode:** Ensure SYSCFG clock is enabled to maintain the high-performance boost.
- **Thermal:** Running at max speed increases power consumption. If the board is placed in an enclosure, it may require passive cooling (heatsink) or better airflow.

### 2. Flash Memory Latency (Wait States)

Flash memory cannot provide instructions at 480 MHz.

- **Rule:** Must maintain `FLASH_LATENCY_4` (4 wait states).
- **Note:** This is based on the AHB (240 MHz) speed. If stability issues occur during heavy memory access, test with `FLASH_LATENCY_5`.
- **Risk:** Dropping to 3 or lower causes the CPU to read garbage instructions → HardFault or infinite loop.

### 3. Bus Speed Limits (The "Gearbox")

The H7 internal buses have lower speed limits than the CPU.

| Bus / Domain | Max Speed | Divider | Actual Speed | Status |
|--------------|-----------|---------|--------------|--------|
| CPU | 480 MHz | PLLP=2 | 400 MHz | Optimal |
| D1 / AHB (HCLK) | 240 MHz | /2 | 200 MHz | Stable |
| D2 / APB1 | 120 MHz | /2 (of AHB) | 100 MHz | Stable |
| D2 / APB2 | 120 MHz | /2 (of AHB) | 100 MHz | Stable |
| D3 / APB3 | 120 MHz | /2 (of AHB) | 100 MHz | Stable |
| D3 / APB4 | 120 MHz | /2 (of AHB) | 100 MHz | Stable |

- **Never** set `AHBCLKDivider = RCC_HCLK_DIV1` if CPU is 400+ MHz.
- **UART8 Note:** Ensure UART8 Init is called after `SystemClock_Config()` because the peripheral clock is now 100 MHz.
- If APB buses are overclocked: UART sends garbled data, timers run at double speed.

### 4. Signal Integrity for High-Speed Pins

At 480 MHz, GPIO switching is more aggressive.

- **GPIO Speed:** For high-speed peripherals (DSI, SDRAM, QSPI), use `GPIO_SPEED_FREQ_VERY_HIGH`.
- **Pull-ups:** At 480 MHz, signal ringing is more common. I2C lines need strong external pull-ups (2.2 kOhm instead of 10 kOhm). Keep SPI traces short.

### 5. Clock Source Stability (HSE vs HSI)

- **HSE:** Using 25 MHz external crystal. `HSE_VALUE` in `stm32h7xx_hal_conf.h` must match the physical crystal.
- **PLL input:** VCO_IN must stay between 8-16 MHz (current: 25/2 = 12.5 MHz). If the crystal ever changes, recalculate PLLM.

### 6. Peripheral Clock Independence

Since we manually manage the clock tree (no CubeMX code generation):

- **Kernel Clocks:** UART8, SAI4, and DFSDM have their own clock sources separate from the system clock.
- **The Conflict:** Changing the main PLL can change peripheral speeds unexpectedly. Always verify `RCC_PeriphCLKInitTypeDef` after a main clock change.
- Check `RCC_D2CCIPR` (UART8, SAI4) and `RCC_D3CCIPR` (BDMA, SAI4 kernel clock) registers.

### 7. DFSDM DMA Sync Strategy — Software Trigger (No Hardware Sync)

In CubeMX: Keep "Enable synchronization" **Unchecked** in the DMA menu.

In Code: Configure the trigger pin as a simple GPIO Input or EXTI (External Interrupt).

The Logic: When the pin goes high (or on software command), manually call:
```c
HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0, RecBuffer, 2048);
```

This is **Option A: Software Trigger** — simpler, more reliable, and recommended for this project.

---

## STM32H7 SAI4 PDM Architecture — Non-Negotiable Rules

These are hard-won lessons from this project. Generic PDM code for STM32 will **not** work here without all five of these satisfied.

### 1. Pin Mapping — Two Different AF Banks

SAI4 uses **two different alternate function banks**. Using AF10 for all pins breaks the internal clock tree.

| Pin | AF | Signal | Role |
|-----|----|--------|------|
| PE2 | AF10 | SAI4_CK1 | PDM clock output to microphone |
| PC1 | AF10 | SAI4_D1 | PDM data input from microphone |
| PE4 | AF8 | SAI4_FS_A | Frame sync (required even in PDM mode) |
| PE5 | AF8 | SAI4_SCK_A | Serial clock (internal bit-shifting reference) |

Reference: UM2411 Section 7.15, Table 15. BSP: `stm32h747i_discovery_audio.h` macros.

### 2. Cache Coherency — D-Cache Invalidation

The Cortex-M7 D-Cache causes stale reads when DMA writes directly to RAM.

**Exception for this project:** `g_PdmBuf` is placed at `0x38000000` (D3 SRAM), which is **outside the cacheable address space**. `SCB_InvalidateDCache_by_Addr()` is therefore **not needed** for the PDM DMA buffer.

If any audio buffer is ever moved to D1 AXI SRAM or D2 SRAM, cache invalidation **must** be added to `HAL_SAI_RxHalfCpltCallback` and `HAL_SAI_RxCpltCallback` before reading the buffer.

All audio buffers must be 32-byte aligned: `__attribute__((aligned(32)))`.

### 3. PDM2PCM Library — Stereo-to-Mono + Correct Call Size

**This is a stereo PDM stream that must be converted to mono PCM.**

SAI4 runs in `SAI_STEREOMODE`. The DMA buffer at `0x38000000` contains **interleaved** D0+D1 words (confirmed by memory view: both slots carry ~50% density PDM data, e.g. `8e8e 5959 8e8e 5959`).

**Do NOT** pass the raw interleaved buffer directly to `PDM_Filter`. The filter will treat D0+D1 as a single mono PDM stream and produce noise because it is alternating between two independent bit-streams.

**Step 1 — De-interleave: extract D1 (odd words) only**
```c
// Mic on PC1=SAI4_D1 → odd words. Take every second word.
for (uint32_t i = 0; i < PDM_MONO_HALF; i++)
    s_pdmMono[i] = pdmSrc[2*i + 1];  // odd = D1 = mic
```

**Step 2 — Call PDM_Filter ONCE per DMA half**

`output_samples_number` MUST equal `AudioFreq / 1000 = 16`. One call per DMA half: 64 D1-only words → 16 PCM samples.

```c
PDM_Filter((void *)s_pdmMono, (void *)s_pcmHalf, &handler);
```

Required filter config:
```c
handler.bit_order        = PDM_FILTER_BIT_ORDER_MSB;
handler.endianness       = PDM_FILTER_ENDIANNESS_LE;
handler.high_pass_tap    = 2122358088u;
handler.in_ptr_channels  = 2u;   // stereo PDM input (D0+D1 interleaved) — library de-interleaves via CRC
handler.out_ptr_channels = 1u;   // mono PCM output
config.decimation_factor     = PDM_FILTER_DEC_FACTOR_64;
config.output_samples_number = 16;   // AudioFreq/1000 — NOT DMA_HALF_SIZE
config.mic_gain              = 24;
```

Buffer sizing constants:
```
PDM_FILTER_CALL_SAMPLES = 16          (AudioFreq/1000)
DMA_HALF_SIZE           = 16          (PCM samples per DMA half = PDM_FILTER_CALL_SAMPLES)
PDM_MONO_HALF           = 64          (D1-only words per half = PDM_BUF_HALF / 2)
PDM_BUF_HALF            = 128         (stereo DMA half, D0+D1 interleaved)
PDM_BUF_TOTAL           = 256         (full stereo DMA circular buffer)
```

Reset the filter with `PDM_Filter_Init()` + `PDM_Filter_setConfig()` before **every** recording to clear stale `pInternalMemory` state.

### 4. Clocking and OSR

| Parameter | Value |
|-----------|-------|
| **Target PCM rate** | 16,000 Hz |
| **PDM decimation setting** | `PDM_FILTER_DEC_FACTOR_128` (OSR = 128) |
| **Required PDM clock** | 16,000 × 128 = 2.048 MHz |
| **Actual SAI4 mic clock** | PLL2P (49.14 MHz) / (MCKDIV=12 × 2) = **2.04750 MHz** (within MP34DT05-A spec 1.2–3.25 MHz) |

PLL2 config used in this project:
- HSE=25 MHz / PLL2M=25 → VCO_in=1 MHz × PLL2N=344 / PLL2P=7 → **49.14 MHz** (SAI4A kernel clock)
- HAL computes MCKDIV=24 → PDM clock = 49.14 MHz / 48 ≈ **1.02381 MHz**
- `AudioFrequency = SAMPLE_RATE × 16 = 256000` (HAL uses this to derive MCKDIV=12)

### 5b. SAI4 PDM Mode — Follow the BSP Exactly (AN5027 Law)

In **SAI PDM mode** (not TDM), the hardware automatically captures both D0 and D1 within a single 16-bit slot. Do NOT manually enable slot 1 — that is TDM thinking applied to PDM mode.

**Authoritative reference:** `MX_SAI4_Block_A_Init` in `stm32h747i_discovery_audio.c`:
```c
hsai->FrameInit.FrameLength    = 16;           // 1 slot × 16 bits
hsai->SlotInit.SlotNumber      = 1;            // ONE slot in PDM mode
hsai->SlotInit.SlotActive      = SAI_SLOTACTIVE_0;  // slot 0 only
hsai->Init.FirstBit            = SAI_FIRSTBIT_LSB;
hsai->Init.ClockStrobing       = SAI_CLOCKSTROBING_FALLINGEDGE;
```

`SLOTR = 0x00010000` is **correct** for SAI4 PDM mode.

**PDM_Filter library rules (AN5027 §3.1 + BSP_AUDIO_IN_PDMToPCM):**
1. `in_ptr_channels = 1` for one mic. Library uses this as byte stride when reading the PDM buffer.
   - BSP pattern: `PDM_Filter((uint8_t*)PDMBuf + index, PCMBuf, &handler)` — cast to `uint8_t*`.
   - For stereo (2 mics): `in_ptr_channels=2`, call twice with `index=0` and `index=1`.
   - **This project: mic is on D1 (PC1) → use `index=1` (odd bytes).** `index=0` extracts D0 (floating — wrong channel).
2. `output_samples_number = AudioFreq/1000 = 16` — always.
3. Call `PDM_Filter_Init()` + `PDM_Filter_setConfig()` before **every** recording.
4. One `PDM_Filter()` call per DMA half — feed `(uint8_t*)pdmSrc`.
5. **Never manually extract or block bits** — pass the raw DMA buffer, cast as `uint8_t*`.
6. **LR=HIGH (SB43 open, R213 pull-up) → mic outputs on FALLING edge → DFSDM Channel 1 (SITP=01)**
   This was confirmed from the board schematic. Do NOT use Channel 1 or rising edge for this board.

### 6. Memory Placement — D3 SRAM for BDMA

SAI4 is in the D3 domain. Its DMA controller is **BDMA**, which can **only access D3 SRAM**.

```c
#pragma location = 0x38000000          // D3 SRAM — only region BDMA can reach
static __no_init uint16_t g_PdmBuf[PDM_BUF_TOTAL];
```

PCM output buffer (`g_AudioBuf`) lives in AXI SRAM (D1, default `.bss`) — written by the CPU task, not BDMA, so this is correct.

---

## DFSDM Channel Selection — Hardware-Confirmed, Non-Negotiable

### The active DFSDM channel is Channel 1 — NEVER Channel 0

### *** THIS BOARD IS WIRED AS LR=HIGH (RIGHT CHANNEL) ***

This was confirmed by reading the board schematic (mb1248-h747i-d04-schematic.pdf, U21 MP34DT05-A RIGHT mic):

**Schematic facts:**
- **SB43 (LEFT SELECTION) = OPEN** — LR pin is NOT pulled to GND
- **R213 (10K)** pulls LR up to the VDD/+3V3 rail
- Therefore **LR = HIGH → RIGHT channel → PDM data on FALLING edge of CLK**
- Required: **SITP=01 (falling edge), ch1_cfg1=0x0000008D**

**MP34DT05-A datasheet rule (confirmed in docs/DFSDM_Implementation_Notes.md):**
- LR = LOW  → rising edge  → DFSDM Channel 0 (SITP=00) ← empty on this board
- LR = HIGH → **falling edge → DFSDM Channel 1 (SITP=01)** ← our mic

**Required register configuration:**
| Register | Field | Value | Meaning |
|----------|-------|-------|---------|
| `DFSDM1_Channel1->CHCFGR1` | SITP[1:0] | `01` | Falling edge — matches LR=HIGH |
| `DFSDM1_Channel1->CHCFGR1` | SPICKSEL[3:2] | `11` | SAI4 internal bridge as clock/data source |
| `DFSDM1_Channel1->CHCFGR1` | CHEN | `1` | Channel enabled |
| `DFSDM1_Channel1->CHCFGR1` | Expected full value | `0x0000008D` | CHEN + SPICKSEL=11 + SITP=01 |
| `DFSDM1_Channel1->CHCFGR2` | DTRBS[4:0] | `6` | Right-shift 6: Sinc3 OSR=125 max ±30,517 fits int16_t |
| `DFSDM1_Filter0->FLTCR1` | RCSEL | CH1 | Regular filter reads Channel 1 |

**Filter selection — Non-Negotiable:**
- Use **DFSDM1 Filter 0** — NEVER Filter 1, 2, or 3.
- `HAL_DFSDM_FilterConfigRegChannel(&hdfsdm1_filter0, DFSDM_CHANNEL_1, DFSDM_CONTINUOUS_CONV_ON)`
- `HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0, g_DfsdmBuf, DFSDM_BUF_TOTAL)`
- Expected FLTCR1 = `0x21240001` (RCHSEL=1, FAST=1, RDMAEN=1, RCONT=1, DFEN=1)

**Why DTRBS must be 6, not 5:**
- Sinc3 OSR=125 maximum output = 125³ = 1,953,125 (21 bits)
- DTRBS=5 → max ±61,035 → overflows int16_t (±32,767) → wraps to garbage DC value
- DTRBS=6 → max ±30,517 → fits int16_t cleanly ✓
- StoreDmaChunk shift: `(int16_t)(src32[i] >> 8)` — correct only when DTRBS=6

**Why SITP=00 (rising edge) fails with LR=HIGH:**
- LR=HIGH mic drives PDM data on falling clock edge only
- SITP=00 (rising) captures the idle half-cycle → all-zeros input to DFSDM
- Sinc3 filter on all-zeros → negative full-scale output = -30518 (DC garbage)
- Fix: SITP=01 (falling) → captures actual mic data

**Global clock (Channel 0 is the clock master — separate from the data channel):**
- `DFSDM1_Channel0->CHCFGR1` bit 31 = DFSDMEN = 1 (global enable)
- bits 22:16 = CKOUTDIV = 24 → CKOUT = APB2(100MHz) / (2×25) = 2.0 MHz ✓
- Expected Ch0 CHCFGR1 full value with global bits: `0x80180000` (no CHEN on Ch0 — Ch0 is clock master only)
- Expected Ch1 CHCFGR1 full value: `0x0000008D` (CHEN + SPICKSEL=11 + SITP=01)

**SAI4 mic clock:**
- PE2 (SAI4_CK1) → 2.048 MHz to mic (PLL2P/24 = 49.14MHz/24)
- SAI4 provides the bit-clock to DFSDM CH1 via internal silicon bridge (SPICKSEL=11)
- `SAI4->PDMCR` must = `0x00000101` (PDMEN + CKEN1) before `__HAL_SAI_ENABLE`

**Mic power:**
- VDD = +3V3 via SB42 (closed) — always powered from CN2 USB, no STPMIC1 init needed
- SB41 = open — STPMIC1 MICBIAS path bypassed

---

## Current System Snapshot

### Audio path
- Mic: onboard MP34DT05-A (PDM MEMS microphone)
- Interface: SAI4_Block_A, PDM master-receive mode
- Format: 16 kHz, 16-bit, mono
- Record window: fixed 3 seconds
- DMA buffer: `g_PdmBuf[128]` in D3 SRAM (0x38000000), via BDMA_Channel1 (MONO: 64 words/half)
- Main audio store: `g_AudioBuf[48000]` in AXI SRAM (aligned 32 bytes)
- Save path: `SDWriteTask` writes `REC_XXX.wav` via FatFS / SDMMC1
- Log path: `RTTLogTask` prints result to SEGGER RTT ch0

### Trigger
- Phase 1: blue button on PC13
- Phase 2: wake word ("Hey Nora")

### USB / debug
- CN2 = ST-LINK debug / power
- CN1 = USB MSC for PC WAV access

---

## Ownership Rules

### DMA ownership
Only one task may own the SAI4/BDMA path at a time.

- Phase 1: `VoiceRecTask`
- Phase 2: `WakeWordTask` listens, then hands off to `VoiceRecTask`

`WakeWordTask` must stop DMA before notifying `VoiceRecTask`.

### SD card ownership
The SD card must not be accessed by unrelated tasks while recording, saving, or streaming is in progress.

Respect `g_SysMode` and any busy flags already used for SD protection.

---

## Key Task Rules

### VoiceRecTask
- Starts SAI4 BDMA (`HAL_SAI_Receive_DMA`)
- Drains audio actively during the 3-second window
- Converts PDM → int16 PCM via PDM2PCM library, stores in AXI SRAM
- Stops DMA
- Signals `SDWriteTask`

### SDWriteTask
Required sequence:
1. Set saving state
2. Freeze sample count immediately
3. Open file
4. Increment file index only after successful open
5. Write WAV header
6. Write PCM payload
7. Close file
8. Log exact result code
9. Return state to idle

### RTTLogTask
- Lowest priority task
- Must not interfere with recording or saving
- Logs exact result code and filename

---

## Logging Standard

All runtime diagnostic prints MUST use `RLOG()` — defined in `CM7/Core/Inc/log_mutex.h`.

`RLOG(fmt, ...)` — timestamped printf-style output to **both** RTT (J-Link RTT Viewer) and SWO (IAR Terminal I/O) simultaneously. Every message automatically gets `[T+%7lu]` timestamp prefix.

```c
RLOG("[REC] file=%s  size=%lu bytes", filename, size);
// Output: [T+  66250] [REC] file=REC_001.wav  size=96044 bytes
```

### Macro summary (log_mutex.h)

| Macro | RTT | Terminal I/O | Timestamp | printf format |
|-------|-----|--------------|-----------|---------------|
| `RLOG(fmt, ...)` | ✅ | ✅ | ✅ | ✅ | ← **use this for all new prints** |
| `LOG(fmt, ...)` | ❌ | ✅ | ❌ | ✅ | legacy / GUI only |
| `LOG_TS(fmt, ...)` | ❌ | ✅ | ✅ | ✅ | legacy |
| `RTT_TS(msg)` | ✅ | ✅ | ✅ | ❌ | fixed strings only |
| `SD_LOG(fmt, ...)` | ✅ | ✅ | ✅ | ✅ | audio_sd.c internal |

### Rules
- Always use `RLOG()` for new diagnostic prints in C files.
- Do NOT use `printf()` directly.
- Do NOT use `RTT_TS()` for new code — use `RLOG()` instead.
- `SD_LOG` stays in audio_sd.c — do not replace it.
- GUI/TouchGFX files may use `LOG()` for simple messages.


---

## LCD / DSI Command Mode / FUIF — Session Findings (2026-05-03)

### The visible bug

After fixing Bug X (832-cushion) and Bug Y (`s_mjpeg_decode_active` guard), the panel showed half-blue-correct / half-green-stale split exactly at the LEFT/RIGHT 400-pixel boundary.

### Pipeline architecture (DSI Command Mode + GRAM)

```
[CPU/DMA2D writes] -> SDRAM framebuffer (0xD0000000, 832×480×3) ->
LTDC reads (832-pitch) -> DSI host -> OTM8009A panel internal GRAM (split LEFT 0–399 / RIGHT 400–799) -> physical pixels
```

- **Single buffer** — `setFrameBufferStartAddresses(frameBuf, 0, 0)`. DMA2D writes the same buffer LTDC reads.
- **GRAM panel** — OTM8009A retains image after one transfer; LTDC clock gates off between frames.
- **LEFT/RIGHT split** — DSI Command Mode requires two DSI transactions per frame with CASET command between halves. Custom code in `TouchGFXHAL.cpp` orchestrates this via `HAL_DSI_EndOfRefreshCallback`.
- **Inherited from ST** — the LEFT/RIGHT split logic comes from the official STM32H747I-DISCO TouchGFX Application Template.

### Confirmed fixes (verified by RM0399 / docs)

| Fix | File | Why |
|---|---|---|
| Framebuffer width 832 (was 800) | `TouchGFXGeneratedHAL.cpp:62` | Matches LTDC `CFBLR` pitch of 832×3 = 2496 B/row in `TouchGFXHAL.cpp:530`. Without it, LTDC drifts +32 px/row → diagonal slicing (Bug X). |
| R↔B byte swap of `s_rgb888Scaled` | `jpeg_decoder.c` post-scale | Per RM0399 §33.7.18 Table 276, LTDC PF=RGB888 reads memory in `[B,G,R]` byte order; jpeg_utils writes `[R,G,B]`. Without swap, blue source → green panel. |
| DSI/LTDC IRQ priority 7 → 5 | `stm32h7xx_hal_msp.c:435,629` | Was preempted by MDMA/SDMMC1/EXTI/JPEG/BDMA mid-handoff. Floor is `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY = 5` — going lower (4 or below) breaks FreeRTOS API in EOR ISR. |
| LTDC_ER ISR enabled + handler | `main.c` MX_LTDC_Init + `stm32h7xx_it.c` | Catches FUIF / TERRIF; without it those errors stay pending forever in NVIC because IER was 0. |

### Root cause of remaining half-blue/half-green

**LTDC FIFO Underrun (FUIF).** The peripheral-level error log via the `LTDC_ER_IRQHandler` shows FUIF firing 3-5 times per LEFT/RIGHT half during DMA2D blits. SDRAM cannot deliver pixels fast enough to LTDC's burst windows when DMA2D is concurrently writing the framebuffer. LTDC sends garbage to DSI mid-scan → corrupt panel GRAM → frozen until next clean transfer.

### Per-second IRQ pattern (instrumented logger at 0x24008000)

| Sec | DSI(123) | LTDC_ER(89) | DMA2D(90) | Notes |
|---|---|---|---|---|
| 0 | 61 | **16** | 1 | Boot frenzy: JPEG decode + DMA2D + first LTDC scans = saturated SDRAM |
| 1 | 59 | 0 | 0 | Steady state — clean 60 Hz DSI, no FUIF |
| 2 | 60 | 0 | 0 | Same |
| 3 | 49 | **9** | 1 | Periodic re-inject's DMA2D blit triggers another FUIF burst |

Conclusion: **FUIF correlates 1:1 with active DMA2D blits.** When DMA2D is idle, no FUIF.

### Approaches tested and what works / doesn't

| Lever | Effect | Verdict |
|---|---|---|
| MPU SDRAM region (Normal Non-Cacheable Bufferable) | None on FUIF | MPU is **CPU-only**; bus masters bypass it. |
| FMC `RPIPE_DELAY` 0 → 1 | FUIF count slightly down | Marginal, low-risk hardening. |
| NVIC priority bump DSI/LTDC 7 → 5 | Eliminates FreeRTOS-tier preemption | Keeps the EOR handoff atomic re: FreeRTOS ISRs but doesn't help bus contention. |
| `lockDMAToFrontPorch(true)` always | DMA2D fully starved → LCD goes black | **Too aggressive.** With LTDC scanning continuously at 60 Hz there's no front-porch window. |
| `lockDMAToFrontPorch(refreshRequested)` (default dynamic) | DMA2D runs, but FUIF still fires during contention | **Current setting.** Safer than full lock. |
| `HAL_Delay` between LEFT and RIGHT EOR | None on FUIF | FUIF happens **mid-scan**, not between halves. |
| Lower TIM6_DAC priority | Priority 15 → black screen (HAL_Delay deadlock in EOR ISR); priority 3 → no improvement | Don't touch. |
| PLL3R clock divider | Bricks app at boot per `project_pll3r_change_bricks_app.md` | Forbidden lever. |
| Move framebuffer to internal RAM | Doesn't fit (1.2 MB > 512 KB AXI SRAM max) | Not an option. |

### Architectural fixes that would actually solve FUIF

The single-buffer + DSI Command Mode + 800×480 RGB888 + AHB3 master sharing combination is at the bandwidth limit. To fully eliminate FUIF requires one of:

1. **DSI Video Mode** — eliminates the LEFT/RIGHT split entirely; LTDC streams continuously with timing margin. Requires CubeMX DSI peripheral reconfig + LTDC timing change + OTM8009A re-init in video mode. ST community has a documented migration guide.
2. **RGB565 instead of RGB888** — halves SDRAM bandwidth demand. Affects color quality minimally for thumbnails, but requires DMA2D / Bitmap format / panel format changes.
3. **Partial framebuffer in internal RAM** — major refactor; no ST template support for STM32H747I-DISCO + GRAM panel.

### Key memory addresses for live debugging

| Region | Address | Notes |
|---|---|---|
| Framebuffer | `0xD0000000` | 832×480×3 ≈ 1.2 MB, single buffer |
| `s_ycbcrBuf` | `0xD0124800` | JPEG decoder MCU output |
| `s_rgb888Buf` | `0xD02E6800` | Post-color-convert intermediate |
| `s_rgb888Scaled` | `0xD0589800` | 800×480 RGB888 source for DMA2D blit |
| **IRQ logger ring** | **`0x24008000`** | 256-entry × 8 B (irq_num + cycle); read via `mem8 0x24008000 2048` |
| **IRQ logger index** | **`0x24008800`** | running count of logged IRQs |

### Confirmed-irrelevant theories (don't re-investigate)

- **PE2/A23 hardware corruption from mic clock** — PE2 is in Analog mode (`GPIOE_MODER` bits[5:4] = `11`); IS42S32800J SDRAM only uses A0–A11; FMC's A23 isn't driven during SDRAM transactions.
- **CM4 zombie interference** — `FLASH_OPTSR2_CUR` BCM4 bit = 0; CM4 doesn't boot. Verified via J-Link CM4 register read.
- **MPU misconfiguration for SDRAM** — added MPU region 4, no effect on FUIF (bus masters bypass MPU).
- **FreeRTOS TIM6 priority** — default works; priority 15 deadlocks `HAL_Delay()` in EOR ISR; priority 3 is no-op.
- **NVIC pending IRQ pipe overflow** — at 100 ms sample intervals, NVIC pipe is consistently empty; no IRQ backlog.

### Tools landed in this session (keep)

- **LTDC_ER ISR + RTT log** (`stm32h7xx_it.c`) — captures FUIF/TERRIF in real time.
- **DMA2D dimension logger** (`STM32DMA.cpp`) — logs every blit's `nSteps`, `nLoops`, strides, src/dst.
- **EOR firing counter** (`TouchGFXHAL.cpp` `HAL_DSI_EndOfRefreshCallback`) — `[LCD-EOR] L=x R=y` per frame.
- **NVIC IRQ ring buffer** (`stm32h7xx_it.c` at `0x24008000`) — ETM-substitute trace; 256 entries, no-wrap (captures only first 256 IRQs from boot for startup analysis).

### Task-list pattern for future LCD debugging on this board

1. **Read RM0399 §33 (LTDC) FIRST** — register byte order in Table 276 alone has caught this bug class twice.
2. **Check `LTDC_ISR` (0x50001038)** for FUIF/TERRIF flags before assuming software bug.
3. **Read `RCC_APB3ENR` (0x58024558)** — if 0, LTDC is gated off (normal in DSI Command Mode between transfers).
4. **Capture IRQ ring buffer** at `0x24008000` for the actual interrupt timeline.
5. **Don't touch PLL3** without explicit revalidation.

### Recommended Late-Start initialization sequence (post 2026-05-04 Gemini guidance)

Premise: the 15 boot-time FUIFs we couldn't eliminate fire **before** any rendering loop runs — during the simultaneous CPU work (memset zeroing the 1.2 MB framebuffer, JPEG decode, asset loads) AND the LTDC's first scans. If LTDC isn't enabled until those CPU-heavy tasks are done, FUIF is impossible (the FIFO doesn't exist yet).

**Restructured init order** for `main.c` / RTOS startup task:

1. **Fundamentals** — RCC clock tree, FMC/SDRAM init, GPIOs. Bus is quiet, no peripherals demanding pixels.
2. **Noisy CPU tasks** — DO ALL of these BEFORE LTDC starts:
   - Zero the 1.2 MB framebuffer at `0xD0000000` (memset or DMA2D Fill)
   - JPEG decode (HW + convertFn)
   - R↔B swap of `s_rgb888Scaled`
   - Load static UI assets into SDRAM
   - Any cache flushes / SCB_CleanInvalidateDCache
3. **Quiet the bus** — short delay (~5 ms) to let any lingering MDMA / DMA2D / cache writebacks drain.
4. **Start the conductor**:
   - `MX_LTDC_Init()` (sets timing, enables Layer 1, but does NOT start scan — DSI Command Mode means scan happens only on `HAL_DSI_Refresh()`)
   - `MX_DSI_Init()` (DSI host config)
   - `HAL_DSI_Start()` — DSI host comes alive
5. **First refresh** — call `HAL_DSI_Refresh()` for LEFT half, then RIGHT half. Framebuffer is already "perfect" from step 2. SDRAM is uncontested. **First scan completes without FUIF.**

**Backlight trick**: keep the panel backlight (PWM or GPIO) OFF until step 5 completes. User never sees the boot corruption — first visual frame is crisp.

**Verification**: the IRQ ring buffer at `0x24008000` should show `LTDC_ER (89)` count = 0 even in second 0 with this sequence. If it's still firing during step 5, something earlier is still active (lingering MDMA, etc.).

**Status as of 2026-05-04**: this restructuring is **NOT YET IMPLEMENTED**. Current code calls `MX_LTDC_Init()` early and `HAL_DSI_Refresh()` happens implicitly via TouchGFX's first-frame paint. To do this properly:
- Move LTDC/DSI init out of the early-init chain
- Hook into TouchGFX's first-frame trigger or override
- Defer backlight enable to the same late point

### CHOSEN ARCHITECTURE — Safe-Boot Sequence (Option A, decided 2026-05-04)

**Decision:** Defer the **entire TouchGFX engine and GUI task** until after the JPEG decode + buffer prep is complete. This creates a "Protected Window" where the Cortex-M7 is the only master on SDRAM during the heavy work — no framework rendering, no DMA2D blits, no LTDC scans competing for AXI bandwidth.

**Why Option A over Option B:**
- Option B (gate the scan triggers) keeps the TouchGFX render task alive — it'll still hammer SDRAM with widget rendering, blits, and label invalidates that compete with JPEG decode for AXI cycles.
- Option A makes the M7 the *only* SDRAM master during the boot bulk-load. AXI arbitration becomes trivial. FUIF cannot fire because LTDC isn't running yet.
- Predictable timing: the WFI-instrumented JPEG loops execute with no interruption from framework code.
- Clean handover: when LTDC finally starts, framebuffer is already "perfect" — first scan-out is correct.

**Refined model (2026-05-04): GATED MULTITASKING — Two phases, three tasks**

A purely sequential boot is insufficient because TouchGFX, JPEG, and Video are inherently concurrent in the running app. Instead, split into:

#### Phase 1 — The "Silent" Boot (Pre-OS)

Before `osKernelStart()`, perform operations that need 100% of the AXI bus without interference:

1. **Hardware Foundation** — Initialize Clocks, FMC (SDRAM), MPU regions.
2. **Splash Load** — Perform initial JPEG decode and R↔B swap for the splash screen image **while LTDC is physically Disabled** (`LTDC_GCR.LTDCEN = 0`).
3. **Video Buffer Prep** — Zero the video ring-buffers in SDRAM with a standard `memset`. Since LTDC isn't scanning, this high-speed bulk operation cannot trigger FUIF.

**End-of-Phase-1 state:** SDRAM holds a complete, correct splash framebuffer at `0xD0000000`. Video buffers are cleared. Backlight still OFF.

#### Phase 2 — Task-Based Launch (RTOS Startup)

Once the clean frame is staged in SDRAM, the FreeRTOS kernel starts and spawns the three core tasks:

| Task | Priority | Responsibility |
|---|---|---|
| **`guiTask`** (TouchGFXTask) | `osPriorityHigh` | Owns LTDC + DSI hardware. Runs the TouchGFX engine. Triggers `HAL_DSI_Refresh()` per frame. EOR callback handles LEFT/RIGHT split. |
| **`videoTask`** | `osPriorityAboveNormal` | Decodes video frames into the secondary SDRAM buffer (the one Phase 1 cleared). Blocks on a "frame request" semaphore when idle. |
| **`jpegTask`** (JpegDisplayTask) | `osPriorityNormal` | Background decoding of UI assets, icons, gallery thumbnails. Runs WFI-yield-instrumented loops so its SDRAM bursts can stand down when LTDC scans. |

**The "gating" between tasks is what eliminates contention:**
- `guiTask` is highest priority — when LTDC needs to refresh, it preempts everything.
- `jpegTask` and `videoTask` use `wait_for_ltdc_idle()` checkpoints (already in `jpeg_decoder.c`) to step out of the bus during scan windows.
- The framework's TE→EOR cycle drives the heartbeat; the lower-priority tasks fill the gaps.

#### Backlight reveal

The backlight GPIO stays LOW from boot through the end of Phase 1 and the first frame in Phase 2. The first time `guiTask` completes a successful EOR for both halves, it toggles backlight HIGH. **User never sees the boot transition** — display goes from dark to crisp first frame.

#### Verification criteria

- IRQ ring buffer at `0x24008000`: `LTDC_ER (89)` count = **0** through seconds 0-3
- Visual: panel goes black → clean first frame, no flash of half-blue/half-green corruption
- Steady state (sec 1+): continued zero FUIF (already proven by WFI yields)
- All three tasks reach their idle/blocked state with stable stack high-water marks

#### Files to change for implementation (not yet done)

| File | Change |
|---|---|
| `CM7/Core/Src/main.c` | **Phase 1 in `main()` pre-`osKernelStart()`:** Clocks + FMC + MPU + initial JPEG decode + video buffer memset, ALL while LTDC is disabled. **Phase 2:** task creation only. Move `MX_LTDC_Init()`/`MX_DSIHOST_DSI_Init()`/`MX_TouchGFX_Init()` into `guiTask` body. |
| `CM7/Core/Src/music_display_task.c` | `jpegTask` (currently `JpegDisplayTask`) keeps existing WFI yield instrumentation; priority lowered to `osPriorityNormal`. |
| `CM7/Core/Src/main.c` (videoTask) | **Re-enable** the currently commented-out videoTask creation (line 384). Priority set to `osPriorityAboveNormal`. Even if no MJPEG widget is active, the task framework should be in place. |
| `CM7/TouchGFX/target/TouchGFXHAL.cpp` | `TouchGFX_Task` body restructured: do `MX_LTDC_Init()` + `MX_DSIHOST_DSI_Init()` + `HAL_DSI_Start()` first, then loop the framework. Backlight GPIO HIGH after first successful EOR. Priority bumped to `osPriorityHigh`. |
| `CM7/Core/Src/stm32h7xx_hal_msp.c` | Backlight GPIO configured as output, default LOW in early boot. |

---

## C-SPY Macro and cspybat Syntax Reference

This section is the authoritative reference for IAR C-SPY macro syntax in this project. All entries verified against the IAR C-SPY Debugging Guide (UCSARM-26 v9.50.x); local PDF in `docs/EWARM_DebuggingGuide.ENU.pdf`.

### PART 1: cspybat Command Line

#### How to invoke

cspybat is invoked through a pre-generated `.cspy.bat` file that IAR creates when you build the project in the IDE. The generated file lives in:

```
<project>/settings/<config>.cspy.bat
```

It already contains all the device, driver, and probe arguments. You only need to add the macro and redirect the log:

```bat
call settings\Debug.cspy.bat firmware.out --macro=macros\my_macro.mac > logs\latest.log 2>&1
```

Always redirect both stdout and stderr (`> log 2>&1`). `__message` output goes to stdout.

#### Useful cspybat flags (after the .cspy.bat call)

- `--macro=<file.mac>` — register and run a macro file at session start
- `--leave_target_running` — when cspybat exits, leave CPU running (does NOT control when cspybat exits)
- `--download_only` — flash and exit immediately (don't run macros or debug)
- `--attach_to_running_target` — connect without resetting

#### Common mistakes

- Calling `cspybat.exe` directly without the `.cspy.bat` wrapper — you'd have to specify driver DLLs, device files, and 20+ flags by hand
- Forgetting `2>&1` — error messages go to stderr and are lost
- Assuming `--leave_target_running` keeps cspybat alive longer (it doesn't)

### PART 2: C-SPY Macro Language

#### File structure

A `.mac` file contains:
- File-scope variables (declared by simple assignment at the top, no `__var` keyword needed at file scope)
- Reserved hook functions (called automatically by the debugger)
- User-defined macro functions (called from hooks or as breakpoint actions)

There is **no preprocessor** — `#define`, `#include`, `#ifdef` do NOT exist.

#### Variables

```c
// File-scope: declared by assignment
bp_handle = 0;
addr_state = 0;

myFunction()
{
    __var local_var;        // Function-scope: __var keyword required
    local_var = 42;
}
```

#### Statements and control flow

C-like syntax: `if`, `else`, `while`, `for`, `do-while`, `return`. Standard C operators (`==`, `&&`, `||`, `<<`, etc.).

#### Reserved hook functions (called automatically — DO NOT call yourself)

| Hook | When it fires |
|---|---|
| `execUserPreload()` | Before firmware is downloaded |
| `execUserSetup()` | Once, after firmware is loaded |
| `execUserExecutionStarted()` | Every time CPU resumes |
| `execUserExecutionStopped()` | Every time CPU halts |
| `execUserPreReset()` | Before a reset is issued |
| `execUserReset()` | After a reset completes |
| `execUserExit()` | Debug session ending |
| `execUserFlashInit()` | Before flash programming |
| `execUserFlashExit()` | After flash programming |

Use `execUserSetup` for installing breakpoints and `execUserExit` for cleanup. The debugger handles run/halt — you don't.

### PART 3: System Macros (verified signatures)

Functions starting with `__` are built-in. Use ONLY the ones listed here. Do not invent functions by analogy with GDB or other debuggers.

#### Output / logging

```c
__message argList;             // Statement, not function. Goes to Debug Log / stdout.
__fmessage file, argList;      // Same, but writes to a file opened with __openFile
__smessage argList;            // Returns the formatted string
```

Format specifiers (use with `value:%spec`):
- `%d` — decimal
- `%x`, `%X` — hex (lower / upper case)
- `%08X` — zero-padded 8-digit hex
- `%c` — character
- `%b` — binary

Example:
```c
__message "RCC->CR = 0x", val:%08X, "\n";
```

#### Breakpoints

```c
handle = __setCodeBreak(location, count, condition, conditionType, action);
handle = __setDataBreak(zone, address, size, accessType, condition, conditionType, action);
__clearBreak(handle);
```

`__setCodeBreak` parameters:
- `location` — string. `"{file.c}.388"` for source line, `"main"` for function, `"main+0x10"` for offset
- `count` — integer. Skip count. `0` = break every time
- `condition` — C expression as string. `"1"` = unconditional. `"buf_index == 42"` = conditional
- `conditionType` — `"TRUE"` or `"CHANGED"`
- `action` — string of macro code to run on hit. `""` = no action. `"myMacro()"` = call myMacro

`__setDataBreak` parameters:
- `zone` — almost always `"Memory"`
- `address` — integer (for a C symbol, use `&symbol` to get its address)
- `size` — `1`, `2`, or `4` bytes
- `accessType` — `"R"`, `"W"`, or `"RW"`
- `condition`, `conditionType`, `action` — same as `__setCodeBreak`

Both return a non-zero handle on success, **0 on failure**. Always check.

#### Memory access

```c
val = __readMemory8(address, "Memory");
val = __readMemory16(address, "Memory");
val = __readMemory32(address, "Memory");
val = __readMemory64(address, "Memory");
__writeMemory8(value, address, "Memory");
__writeMemory16(value, address, "Memory");
__writeMemory32(value, address, "Memory");
__writeMemory64(value, address, "Memory");
```

Note argument order in writes: **value first, then address**.

#### Symbol resolution

In C-SPY macros, C symbols can be referenced **directly** as expressions. A bare symbol name evaluates to the *value* at that symbol; prefix with `&` to get the *address*. The debugger handles symbol-to-address resolution behind the scenes because it loaded the .out/ELF.

```c
val   = g_state;                              // Direct read: value of g_state
addr  = &g_state;                             // Direct address-of: address of g_state
val   = __readMemory32(&g_state, "Memory");   // Same value via memory read

// For function symbols (used with __hwRunToBreakpoint, etc.):
__hwRunToBreakpoint(&main, 5000);             // Run until main() entry

// __symbolAddress("name") is NOT in IAR EW v9.4.6 -- use & directly.

// Dynamic / string-based resolution:
result = 0;
__evaluate("g_state",  &result);              // result = value of g_state
__evaluate("&g_state", &result);              // result = address of g_state
defined = __isMacroSymbolDefined("name");     // True if symbol exists
```

#### Execution control (use sparingly)

```c
__delay(ms);                              // Pauses MACRO on host, NOT target
__hwReset(halt_delay_ms);                 // HW reset + halt
__hwResetWithStrategy(halt_delay, strategy);
__hwRunToBreakpoint(address, timeout_ms); // address must be integer
__hwResetRunToBp(strategy, address, timeout_ms);
```

**Important:**
- `__delay` does NOT halt the target — it pauses macro execution on the host. The target keeps running (or stays halted) — whatever it was doing continues.
- `__hwRunToBreakpoint` returns: `>=0` time-to-hit, `-1` BP install failed, `-2` timeout
- Address arguments to `__hwRunToBreakpoint` and `__hwResetRunToBp` must be **integers**. For a C symbol, prefix with `&` (e.g. `__hwRunToBreakpoint(&main, 5000)`). The function `__symbolAddress()` is NOT available in IAR EW v9.4.6.

#### File I/O (host side)

```c
fh = __openFile("path\\to\\file.txt", "w");  // "r", "w", "a"
__writeFile(fh, value);
__writeFileByte(fh, byte);
val = __readFile(fh);
val = __readFileByte(fh);
__resetFile(fh);
__closeFile(fh);
```

Use `__fmessage fh, args;` to write formatted output to a file.

#### Misc useful

```c
__isBatchMode()                  // True when running under cspybat
__wallTime_ms()                  // Host wall-clock in ms
__getSelectedCore()              // For multicore
__abortLaunch()                  // Force end the session
```

### PART 4: Functions That Do NOT Exist

DO NOT use these. They are common false friends from GDB or other debuggers:

- `__go()` — does not exist. The debugger handles "go" implicitly.
- `__stop()` — does not exist.
- `__halt()` — does not exist.
- `__break()`, `__continue()` — do not exist.
- `printf(...)` — use `__message` instead.
- `#define`, `#include` — no preprocessor at all.

If you need to "drive execution," use the lifecycle hooks (`execUserSetup`, etc.) or `__hwRunToBreakpoint`. The CPU runs by default; you don't tell it to.

### PART 5: Canonical Macro Template

Use this as the starting point for any new `.mac` file:

```c
// my_macro.mac
// Purpose: <describe>

bp = 0;

execUserSetup()
{
    __message "===== SESSION START =====\n";
    bp = __setCodeBreak("{main.c}.42", 0, "1", "TRUE", "onHit()");
    if (bp == 0)
        __message "[ERROR] BP install failed\n";
    else
        __message "[OK] BP installed, handle=", bp, "\n";
}

onHit()
{
    __message "[BP-HIT] Reached BP\n";
    __message "[REG] RCC->CR = 0x",
              __readMemory32(0x58024400, "Memory"):%08X, "\n";
}

execUserExit()
{
    __clearBreak(bp);
    __message "===== SESSION END =====\n";
}
```

### PART 6: Reading the Log

After every cspybat run, read the log file. Look for:

- `===== SESSION START =====` — confirms macro executed
- `===== SESSION END =====` — confirms clean exit. **If missing, the macro crashed mid-run.**
- `[ERROR]` — your own logged errors
- `[OK]` — your own logged successes
- cspybat / driver errors near the top of the log if connection failed
- Any line starting with `Fatal error:` from cspybat itself

If the log is empty: the macro probably has a syntax error. cspybat usually reports it in the first few lines.

### When in Doubt

- If a function isn't listed here, search the IAR C-SPY Debugging Guide PDF in the project's `docs/` folder before using it.
- Verify exact signatures by searching for the function name in the PDF — IAR's docs are authoritative.
- If something doesn't work, the first debugging step is reading the log carefully — not adding more code.

### Project-discovered specifics

- **Format string lexer quirk:** width-with-leading-zero (`%02d`) is parsed as octal literal `02` and fails. Use plain `%d` or `%2d`.
- **SDRAM unreadable pre-FMC-init:** reading `0xD0000000` before `MX_FMC_Init` runs returns a bus-fault from the DAP, which the macro engine reports as `Operation error` and aborts the session. Only read SDRAM at checkpoints AFTER `MX_FMC_Init`.
- **`__hwRunToBreakpoint` driver support per UCSARM-26:** CMSIS-DAP, I-jet, J-Link/J-Trace, PE micro, ST-LINK, TI XDS. Confirmed working with J-Link Ultra V7 / J-Link Commander V9.34b on this project.
- **Sequential RunToBp pattern is the working multi-checkpoint flow:** put the entire probe sequence inside `execUserSetup`. Each `__hwRunToBreakpoint(addr, timeout)` advances the CPU to the next checkpoint. After all checkpoints, the macro returns and the session ends cleanly.
- **One useful BP hit per cspybat session (passive `__setCodeBreak` model):** `__setCodeBreak` lets you arm 15+ BPs in a single `execUserSetup` call — install always succeeds. But once the target is running, only the FIRST BP that fires reliably runs its action and produces useful output. Subsequent BPs in the same session are unreliable: cspybat in this environment doesn't dependably resume the CPU after the first hit's action returns. **Mental model:** install N BPs if you like, but expect ~1 useful hit per cspybat invocation. For multi-checkpoint mapping, run cspybat N times via an orchestrator (`boot_pipeline_lcd.ps1`, `verify_pfb.ps1`) — each iteration generates a per-checkpoint `.mac` from a `_one_bp.mac.tpl` template via `@@CHECKPOINT@@` / `@@SKIP@@` substitution and installs ONE intended BP. This differs from the active `__hwRunToBreakpoint` flow above (where many checkpoints DO work in one session) because passive `__setCodeBreak` relies on natural execution to hit the BP, and cspybat's post-action resume path doesn't behave the same as `__hwRunToBreakpoint`'s explicit advance.
- **Stop-and-fix at the first failing BP — don't keep running checkpoints against broken infrastructure.** When a BP-bisect orchestrator runs N checkpoints sequentially (`verify_pfb.ps1` etc.), the moment one BP fails to fire (cspybat times out / chip locks / target hangs before reaching the symbol), all downstream BPs in the sequence are gated on the same broken state and will also fail. There is no diagnostic value in continuing past the first failure — every later iteration burns ~30 s of host watchdog only to confirm the same hang. **Correct methodology:** run iteration N → if it fires cleanly, advance to N+1; if it fails, STOP, read the per-iteration log, fix the root cause in firmware, rebuild, and re-run from N (or N-1 to confirm regression). The orchestrator script's `Wait-Job -Timeout` is a watchdog for cspybat hangs, not a green light to plough through a broken pipeline.

### LTDC reads alias when CPU halted post-init (chip-level behavior, NOT a probe bug)

**Discovered 2026-05-04.** When the CPU is halted at any post-`MX_LTDC_Init` checkpoint (e.g., `MX_TouchGFX_Init` entry, `touchgfx_taskEntry` entry, `HAL_DSI_Refresh` entry), reads to ALL 11 LTDC layer registers return the same value (`0xC0002220` = the GCR register's value). DSI / DMA2D / RCC / FMC / MPU / NVIC / GPIO at the same halt point all read distinct values — only LTDC aliases.

**Verified apples-to-apples** that this is not a cspybat artifact:
- `boot_pipeline_cspy.bat` halts at `MX_TouchGFX_Init` entry via `__setCodeBreak` action, reads via `__readMemory32` → all 11 LTDC regs alias to `0xC0002220`
- `boot_pipeline_jlink_compare.bat` halts at the *same* address (`0x08026EA8`) via J-Link Commander `setbp 0x08026EA8` + `g`, then `mem32` reads → **identical aliased values**
- Both probes confirmed halting at the same PC by reading `regs` (PC=0x08026EA8 in both)

**Root cause:** Both halts use the same M7 Debug-HALT primitive (`DHCSR.S_HALT=1`, set either by FPB match or by DAP write). The M7 core freezes, but **`DBGMCU` on STM32H7 has no freeze bit for LTDC** (per RM0399 §60.5) — LTDC keeps scanning during CPU halt. The active LTDC AHB-master traffic into APB3 register space contends with the DAP's read transaction, and the bus returns `0xC0002220` (the most-recent successful read) for subsequent register accesses.

**Same probe at `MX_LTDC_Init` entry (BEFORE LTDC is enabled): reads cleanly — all 11 LTDC regs read `0`, distinctly.** The aliasing is only after LTDC is actively scanning.

**Practical workarounds when probe needs accurate LTDC state post-init:**

1. **Read the configuration via C-side variables, not the live LTDC registers.** TouchGFX/HAL keeps the configured values in RAM (e.g., `hltdc.Init.HorizontalSync`, `hltdc.LayerCfg[0].FBStartAdress`). Those are in DTCM/AXI SRAM and read cleanly while halted:
   ```c
   __readMemory32(&hltdc.Init.HorizontalSync, "Memory")    // works
   __readMemory32(&hltdc.LayerCfg[0].FBStartAdress, "Memory") // works
   ```
2. **Use `boot_pipeline.bat` (J-Link Commander, free-running)** for the LTDC live state. mem32 with the target running succeeds because there's no halt-vs-LTDC bus contention.
3. **Stop LTDC first**, then halt — write `LTDC.GCR.LTDCEN = 0` from the macro before sampling. But this changes peripheral state and breaks any concurrent operation, so it's a destructive last resort.

### cspybat / C-SPY vs J-Link Commander — division of labor

| Capability | cspybat + C-SPY macros | J-Link Commander |
|---|---|---|
| BP at function entry by name | `__setCodeBreak("MX_LTDC_Init", ...)` | Look up `0x08024C70` in `.map` first |
| BP at source line | `__setCodeBreak("{TouchGFXHAL.cpp}.388", ...)` | Impossible — line numbers not visible |
| BP at function offset | `__setCodeBreak("main+0x10", ...)` | Manual decimal-add to function address |
| Read variable by name | `__readMemory32(&g_state, "Memory")` or just `g_state` | Manual address lookup, then `mem32 <addr>` |
| Resolves through optimization | Yes — debug info follows inlining/folding | No — re-extract addresses every build |
| Free-running mem32 sample | ⚠️ Needs explicit halt logic | ✅ `mem32` while target runs |
| Halt-mode register read | ✅ but subject to LTDC-aliasing chip behavior | ✅ with same chip-level limit |
| Symbol table awareness | ✅ from .out/ELF | ❌ |

**Best-tool-for-the-job split in this project:**

| Use case | Tool |
|---|---|
| Boot stage verification (clocks, MPU, peripheral inits, task dispatch) | **`boot_pipeline_multi.bat`** — cspybat 25 checkpoints x 3 lifecycle hooks |
| Quick "is FUIF firing right now?" / framebuffer painted check | **`boot_pipeline.bat`** — JLink Commander free-running mem32 |
| Snapshot at one early checkpoint | **`boot_pipeline_cspy.bat`** — single cspybat call, fast |
| Apples-to-apples cspybat-vs-JLink debug | **`boot_pipeline_jlink_compare.bat`** — diagnostic only |

### Using cspybat + C-SPY macros to debug the LCD half-blue/half-green bug

The Gated Multitasking probe verified Phase 1 silent boot is clean. The remaining LCD bug (half-blue/half-green at the 400-pixel split) is post-Phase-2, during DMA2D blit + LTDC scan. The cspybat tooling we built can probe these stages by setting BPs at:

| BP target | What it tells us |
|---|---|
| `MX_LTDC_Init` final line (just before return) | Verify the `hltdc` struct in RAM matches the expected 800×480 RGB888 config. RAM reads work fine — no aliasing issue. |
| `HAL_DSI_Refresh` entry | Capture state right before each panel refresh. DSI state, framebuffer first row, DMA2D state. Run multiple times to see frame-to-frame consistency. |
| `HAL_DSI_EndOfRefreshCallback` entry | LEFT/RIGHT split happens here. Probe `currFbBase`, `updateRegion`, `displayRefreshing` flags. |
| `LCD16bpp_lineFromRGB888` (or `LCD24bpp_*`) entry | Capture every DMA2D blit's source/dest/length params. If destination address falls outside `0xD0000000-0xD012C000`, that's a bug. |
| `LTDC_ER_IRQHandler` entry | Fires on FUIF/TERRIF. Capture cycle counter, DMA2D state at moment of fault — pinpoints which DMA2D operation triggered the FIFO underrun. |
| `DMA2D_IRQHandler` (any error path) | Catches Transfer Error or Config Error. |

**Macro approach (extends `boot_pipeline_one_bp.mac.tpl`):**

For each BP, the action macro reads:
- DTCM/AXI variables (always work): `hltdc.Init.*`, `hdma2d.Init.*`, `frameBuf[0]`, `frameBuf[100*832*3]`
- DMA2D registers (work at halt): `CR`, `ISR`, `NLR`, `OMAR`, `FGMAR`, `BGMAR`
- DSI registers (work at halt): `CR`, `WCR`, `ISR0`, `IER0`, `VMCR`
- DWT cycle counter for timing
- AVOID reading LTDC layer registers at halt (use C-side struct instead)

**Single-cspybat-invocation pattern (per LCD-debug session):**
1. Set ONE BP at the suspect location
2. Action macro dumps the relevant subsystem state
3. cspybat exits (one-BP-per-invocation limit per CSpyBat 9.4.6.1706)
4. Run again with a different BP for the next stage

**Multi-checkpoint orchestration:**
Reuse `boot_pipeline_multi.bat`'s pattern — list 5-10 LTDC/DMA2D probe points in `boot_pipeline_lcd.ps1`, generate per-iteration `.mac` from the same template, get a chronological dump across the rendering pipeline.

**Specific bug-narrowing protocol for the half-blue/half-green issue:**

1. **Phase 1**: BP at `MX_LTDC_Init` final line → verify `hltdc.LayerCfg[0].FBStartAdress = 0xD0000000`, pixel format = `LTDC_PIXEL_FORMAT_RGB888`. Confirms TouchGFX is configured correctly.
2. **Phase 2**: BP at `HAL_DSI_Refresh` entry → run 5 times, log cycle counter each time. Verify refresh fires at ~16.6ms intervals (60Hz). Check DMA2D `ISR.TEIF`/`CEIF` between refreshes.
3. **Phase 3**: BP at `HAL_DSI_EndOfRefreshCallback` → check `currFbBase` and `updateRegion` values. The LEFT half should refresh from `frameBuf+0`, RIGHT from `frameBuf+1200`. If the values flip or stick on one side, that's the bug.
4. **Phase 4**: BP at `LTDC_ER_IRQHandler` → if it fires, capture DMA2D state. Find which DMA2D blit was active at the moment of FUIF.

The macro template already exists; for LCD debug we'd add a dedicated `boot_pipeline_lcd_one_bp.mac.tpl` with the LCD-specific register set, plus a `boot_pipeline_lcd.ps1` orchestrator listing the LCD-specific BP targets above.

---

## LCD Bus Guard — Robust Gated Wait (Gemini Plan, 2026-05-05)

### Problem

3-task LCD architecture (TouchGFXTask + JpegDisplayTask + videoTask) fires **16 LTDC_ER (FUIF/TERRIF) IRQs in the first 32 IRQs from boot** — measured via the alias-immune IRQ ring buffer at `0x24008000`. Visual: half-blue/half-green corruption at LEFT/RIGHT 400-pixel split.

Root cause: while LTDC is mid-scan (autonomous AHB master, ~5–10 ms per LEFT or RIGHT half), guiTask blocks on `OSWrappers::waitForVSync()`'s message queue. The FreeRTOS scheduler dispatches the next-priority task (`jpegTask` at osPriorityLow/Normal), which performs `Music_InjectTestThumb()` → memcpy → JPEG hardware decode → scale → R↔B swap → DMA2D blit, all hitting SDRAM. CPU+LTDC concurrent SDRAM access → LTDC FIFO underrun → FUIF.

### Why the previous fix attempts failed

| Attempt | Outcome |
|---|---|
| `cbc9523` single-task revert (merge JpegDisplayTask into TouchGFXTask via Music_Poll in Model::tick) | Chip HardFaulted on first run — Music_Poll() called from Model::tick may run before framework is fully initialized; SP corrupted. |
| Unbounded `while (displayRefreshing) { __WFI(); }` after `HAL_DSI_Refresh` | Chip HardFaulted — first refresh's EOR ISR didn't fire (NVIC race during framework startup) → CPU spun in WFI forever → watchdog/assert HardFault → "Stack pointer is setup to incorrect alignment. Stack addr = 0x1" diagnostic from J-Link. |

### The fix (implemented, this commit)

**File:** [`CM7/TouchGFX/target/TouchGFXHAL.cpp`](CM7/TouchGFX/target/TouchGFXHAL.cpp), inside `flushFrameBuffer()` at the LEFT-half `HAL_DSI_Refresh()` call site.

```cpp
displayRefreshing = true;          // set BEFORE the refresh kick (was after)
HAL_DSI_Refresh(hdsi);
// Allow ~20 ms (more than a 60Hz frame) of retries
for (uint32_t timeout = 0; timeout < 5000 && displayRefreshing; timeout++) {
    __WFI();
}
// Post-Wait Safety Check: if EOR ISR was somehow missed, force-clear
// the flag so the system doesn't stay stuck.  Costs at most one corrupt
// frame (RIGHT half won't trigger that frame); permanent CPU hang is not
// recoverable.
if (displayRefreshing) {
    displayRefreshing = false;
}
```

**3 design elements working together:**

1. **WFI gate** — Holds guiTask in M7 halt state during the scan. Because guiTask is at `osPriorityHigh` and the loop keeps it the highest-priority running task, the scheduler doesn't switch to lower-priority tasks. CPU bus master is silent. SDRAM is exclusively LTDC's during the scan. FUIF cannot fire from CPU+LTDC contention.

2. **Bounded `for` loop with 5000-iteration cap** — Each `__WFI()` wakes on the next IRQ (typically SysTick at 1 kHz, plus DSI/LTDC IRQs). 5000 iterations = worst-case ~5 sec; normal scan exits in << 100 iterations once EOR clears `displayRefreshing`. The bound prevents permanent hang if EOR is masked or stalled.

3. **Post-wait safety clear** — If the loop times out with `displayRefreshing` still true, force-clear it. Loses one frame's RIGHT-half scan (visible glitch on that frame), but the system progresses. The guiTask's existing `OSWrappers::waitForVSync()` afterwards still handles frame timing.

### What stays unchanged

- `OSWrappers::waitForVSync()` in `OSWrappers.cpp:113-121` — frame timing between frames. Allows lower-priority tasks to run during the inter-frame idle window. NOT replaced by the WFI gate.
- The 3-task architecture (TouchGFXTask, JpegDisplayTask, videoTask).
- `wait_for_ltdc_idle()` in `jpeg_decoder.c:23-27` — finer-grained per-row protection during decode loops, complementary to this scan-window gate.
- The second `HAL_DSI_Refresh()` call at `TouchGFXHAL.cpp:561` (RIGHT half) — that one is inside `HAL_DSI_EndOfRefreshCallback` (ISR context). NEVER add WFI in an ISR. The single guard at the LEFT-half site naturally covers both halves because `displayRefreshing` stays true until the RIGHT-half EOR completes at line 582.

### Verification protocol

1. **Hardware recovery** before first run (chip may be wedged from a prior unbounded-WFI HardFault):
   - Power-cycle the J-Link USB cable (unplug, wait 3s, replug)
   - Press the black RESET button on the STM32H747I-DISCO board (or unplug + replug CN2 USB)

2. **Build + probe:**
   ```
   EWARM\boot_pipeline_lcd.bat
   ```

3. **Read the IRQ ring buffer** count from any successful checkpoint log:
   ```
   grep "first .* entries" EWARM\runs\boot_pipeline_lcd\refresh_60_early_render.log
   ```

4. **Pass criteria:**

   | Metric | Pre-fix | Pass criterion |
   |---|---|---|
   | `LTDC_ER(FUIF/TERRIF)` in first 32 IRQs | 16 | **0** (or single-digit) |
   | Visual on panel | half-blue/half-green at LEFT/RIGHT split | clean test thumbnail (Debug build has Music_InjectTestThumb) |
   | Voice recording / SD writes / NORA UART | working | still working (priorities unchanged) |

5. **If `LTDC_ER` count is non-zero after the fix:** the timeout fallback fired, meaning EOR was missed for at least one frame. This is recoverable but indicates a deeper synchronization issue. Diagnose by setting BPs at `HAL_DSI_EndOfRefreshCallback` entry and the post-wait safety clear (would need `boot_pipeline_lcd.ps1` extended).

### Theory: why this is the right intervention level

| Layer | What it controls | Effective for FUIF? |
|---|---|---|
| **Single-task merge (cbc9523)** | All LCD work in one thread → naturally serial | Yes (proven historically) but breaks current branch's task structure |
| **Mutex-protected refresh trigger** | Coordinates jpegTask vs guiTask via FreeRTOS primitive | Theoretically yes; risk of priority inversion |
| **`__WFI` gate at refresh site (THIS FIX)** | CPU-level halt during scan; scheduler can't dispatch | Yes — surgical 1-file change, no architecture changes |
| **DBGMCU LTDC freeze bit** | Halt LTDC when CPU halts (debug only) | Doesn't exist on STM32H7 per RM0399 §60.5 |
| **DSI Video Mode migration** | LTDC streams continuously, no LEFT/RIGHT split | Architectural fix, large scope |
| **RGB565 instead of RGB888** | Halves SDRAM bandwidth need | Architectural fix, color quality impact |

The WFI gate is the smallest-scope intervention that targets the root cause (concurrent SDRAM access during scan).

---

## PFB Strip Dispatch — Single-Buffer Serialization Fix (2026-05-06)

### The visible bug

After the PFB Phase-2 milestone (commit `1fd1e71`), the LCD showed only the **top 120 rows** (one strip) of any rendered content; the bottom 360 rows displayed uninitialized OTM8009A panel GRAM (rainbow noise / black). All 4 strips were configured (`setNumberOfBlocks(4)`, `setMaxBlockLines(120)`, `frameBuf` 800×120 in AXI SRAM at `0x24000000`) but only strip y=0 ever reached the panel.

### Diagnosis methodology — the lesson

**Theory-based fixes cost half a day; ITM-trace data found the bug in 30 seconds.** The order that worked:

1. **Add software counters** (`g_pfb_dbg_count`, `g_pfb_dbg_mask`, `g_pfb_dbg_alloc_*`) read via IAR Live Watch to see which functions ran how many times. Reaches verdict: framework calls `allocateBlock` for all 4 y values (`alloc_mask=0xF`) but `transmitBlock` only fires for y=0 (`mask=0x1`).
2. **Add bare-metal ITM port-0 markers** (`pfb_itm.h` → `ITM_PFB('a'/'b'/'c'/'d'/'M'/'g'/'X'/'E'/'F'/'!'/'?'/'B'/'N'/'T'/'t'/'1'`) to get a cycle-accurate event stream over SWO. View in **IAR's SWO Trace window**, port 0 ASCII.
3. **Read the stream literally.** First-frame trace before fix:
   ```
   T B aMM!gX bMM cMM dMM N E FF
   ```
   Decoded: alloc fires for all 4, but transmitBlock (`X`) only fires for the FIRST. Strips 1/2/3 mark DRAWN then nothing — `tryTransmitBlock` bails because `transmitActive()` returns true (s_dsi_busy=true from strip 0 still in flight). After much later EOR, state has been clobbered to ALLOCATED → DRAWN by subsequent allocs, and `freeBlockAfterTransfer` resets to EMPTY before tryTransmitBlockFromIRQ can dispatch the queued strip.

### Two interlocked root causes

**Bug A — `flushFrameBuffer` returned immediately after kicking the transmit.** The framework then called `allocateBlock(y=120)` while strip 0's DSI transfer was still in flight. The `StripAllocator::allocateBlock` unconditionally sets `state=ALLOCATED`, clobbering the SENDING state. Strip 1 sat in DRAWN state but `tryTransmitBlock` bailed because `transmitActive()` was still true.

**Bug B — EOR handler had a tautology that NEVER cleared `s_dsi_busy`:**
```cpp
if (!touchgfx::transmitActive()) {     // transmitActive() reads s_dsi_busy
    touchgfx::s_dsi_busy = false;       // only clears if already false (no-op)
}
```
So even after EOR fired and the previous transfer completed, `s_dsi_busy` stayed `true`, preventing all subsequent dispatches.

### The fix (committed in this session, two files)

**File 1:** [`CM7/TouchGFX/target/TouchGFXHAL.cpp`](CM7/TouchGFX/target/TouchGFXHAL.cpp) — `flushFrameBuffer()` waits for transmit to complete before returning:

```cpp
if (getFrameRefreshStrategy() == REFRESH_STRATEGY_PARTIAL_FRAMEBUFFER)
{
    FrameBufferAllocator* alloc = getFrameBufferAllocator();
    if (alloc) alloc->markBlockReadyForTransfer();
    PartialFrameBufferManager::tryTransmitBlock();

    /* SERIALIZE: wait for DSI scan + EOR ISR + freeBlockAfterTransfer
     * before returning. Otherwise framework's next allocateBlock clobbers
     * SENDING state and only strip 0 ever transmits. */
    uint32_t timeout = 200000u;
    while (touchgfx::transmitActive() && --timeout) { __NOP(); }
}
```

**File 2:** Same file, `HAL_DSI_EndOfRefreshCallback()` — clear `s_dsi_busy` unconditionally + correct ordering:

```cpp
/* 1. Free the just-completed block (state -> EMPTY) */
if (HAL::getInstance()) {
    FrameBufferAllocator* alloc = HAL::getInstance()->getFrameBufferAllocator();
    if (alloc) alloc->freeBlockAfterTransfer();
}
/* 2. Clear FIRST so transmitActive() reports false to tryTransmitBlockFromIRQ */
touchgfx::s_dsi_busy = false;
/* 3. Dispatch next strip if one is queued */
touchgfx::PartialFrameBufferManager::tryTransmitBlockFromIRQ();
```

### Verification — `aMM!gXEFF bMM!gXEFF cMM!gXEFF dMM!gXEFF N`

After fix, SWO Trace shows the full per-strip cycle for each of 4 strips per frame: alloc → mark → ready → getBlock → transmit → EOR → free. Visual: full red background painted across all 480 rows of the panel (verified by setting `__background` to `RGB(255,0,0)` in Screen1ViewBase).

### Lessons for future LCD/PFB debugging

1. **`pfb_itm.h` (added this session) is the right tool for "where does the code actually go?"** Bare-metal ITM port writes, no CMSIS dependency, ~30 ns per call. Visible in IAR's View → SWO Trace.
2. **One-character markers per state transition** beat multi-byte `RLOG` for high-frequency code paths. Pattern recognition at human speed.
3. **Enable "Generate timestamps" in IAR SWO Trace settings** — every ITM packet then carries a cycle stamp from DWT.CYCCNT, hardware-attached.
4. **`ITM_EVENT8(port, value)` and `ITM_EVENT32(port, value)`** (in `pfb_itm.h`) are the right tool when you need numeric data, addresses, or counters per event. Encode numeric values to printable ASCII range (`'0' + n`) or use SWO Trace's hex view, otherwise non-printable bytes look like silence.
5. **`__itm(cycle, port, value)` is a C-SPY macro** (debugger-side, used in `.mac` files at BP hit handlers — NOT a firmware function). Use it when firmware instrumentation is too invasive or rebuild is too expensive.
6. **Single-buffer PFB on STM32H7 + DSI Command Mode requires synchronous flush.** With `setNumberOfBlocks(4)` + 1 physical buffer, the strip dispatch must serialize via a wait in `flushFrameBuffer`. Multi-buffer pipelining would need `frameBuf` to be N strips wide (288 KB × N) which doesn't fit AXI SRAM for N>1.

### Files modified

| File | Change | Status |
|---|---|---|
| `CM7/TouchGFX/target/TouchGFXHAL.cpp` | flushFrameBuffer wait + EOR ordering + ITM_PFB markers | committed candidate |
| `CM7/TouchGFX/target/generated/TouchGFXGeneratedHAL.cpp` | StripAllocator state-machine ITM markers | committed candidate |
| `CM7/TouchGFX/target/generated/OSWrappers.cpp` | static-buffer `osMessageQueueAttr_t` / `osSemaphoreAttr_t` (Hard Rule #9) | committed candidate |
| `CM7/TouchGFX/App/app_touchgfx.c` | TouchGFX_Task lifecycle counters + RLOG | committed candidate |
| `CM7/Core/Inc/pfb_itm.h` | NEW: bare-metal ITM helpers (`ITM_PFB`, `ITM_EVENT8`, `ITM_EVENT32`) | committed candidate |
| `CM7/TouchGFX/generated/gui_generated/src/screen1_screen/Screen1ViewBase.cpp` | red bg for visual test (revert to black before merging) | DEBUG ONLY |
| `CM7/Core/Src/main.c` | task isolation `#if 0` blocks (videoTask, UART, CommandHandler, RtosTrace) | DEBUG ONLY — re-enable |
| `CM7/Core/Src/music_display_task.c` | `Music_InjectTestThumb()` disabled | DEBUG ONLY — re-enable |

### Open work for next session

1. Revert red `__background` → black in Screen1ViewBase
2. Re-enable disabled tasks in main.c (videoTask, UARTReceiveTask, CommandHandler, RtosTrace_DrainTask)
3. Re-enable `Music_InjectTestThumb()` so MusicScreen path runs
4. Verify thumbnail flow still works end-to-end (no regression)
5. Commit as a single logical fix with the trace evidence in the message

---

## SAVE STATE — 2026-05-07 (context-limit checkpoint)

Snapshot of project status at end of 2026-05-07 session, written so a future Claude can resume without losing context. Read this section first when starting fresh.

### Confirmed FIXED (verified end-to-end this session)

| Bug | Mechanism | Verification | Files |
|---|---|---|---|
| **FUIF (LTDC FIFO Underrun)** | Eliminated by moving framebuffer to AXI SRAM (`0x24000000`) and PFB strip strategy. LTDC scans only AXI; FMC bus 100% free for JPEG/CPU/MDMA. | IRQ ring buffer at `0x24050000` shows 0 LTDC_ER (89) IRQs in steady state. SWO trace shows clean strip-by-strip dispatch with no FUIF markers. | `CM7/Core/Src/main.c`, `EWARM/stm32h747xx_flash_CM7.icf` (per commit `1fd1e71`) |
| **Deadlock at first VSync** | `waitForVSync()` blocked forever on `osMessageQueueGet(NULL,...)`. Two factors: (a) `osMessageQueueNew(NULL)` could fail under heap pressure → NULL queue handle. (b) EOR handler had a tautology that never cleared `s_dsi_busy`. | RTT log + IAR Live Watch confirm `wait_unblock` increments now (was stuck at 0). Task wakes per-frame. | `CM7/TouchGFX/target/generated/OSWrappers.cpp` (static `osMessageQueueAttr_t` + `osSemaphoreAttr_t`), `CM7/TouchGFX/target/TouchGFXHAL.cpp` (EOR handler reorder) |
| **Heap exhaustion (Hard Rule #9)** | `osMessageQueueNew/osSemaphoreNew(NULL)` silently dynamic-allocate from `ucHeap`. When BLE_UART_Init's `xQueueCreate` chain consumed the heap, subsequent NULL-attr creators returned NULL handles. | OSWrappers now uses `StaticQueue_t` + `StaticSemaphore_t` storage in BSS. Build clean. No NULL-queue HardFaults. | Same as above (OSWrappers.cpp static-buffer fix) |
| **Strip dispatch (single-strip-only paint)** | Two interlocked causes: (1) `flushFrameBuffer` returned immediately after `tryTransmitBlock`, framework called `allocateBlock(y=120)` while strip 0's DSI scan was still in flight, our 1-buffer allocator clobbered `state=SENDING → ALLOCATED`. (2) EOR's `s_dsi_busy = false` was guarded by a self-referential `if (!transmitActive())`, never cleared. | SWO Trace: `aMM!gXEFF bMM!gXEFF cMM!gXEFF dMM!gXEFF N` per first frame. RTT: 4× `[PFB-TX] y=0/120/240/360` within 7 ms. Visual: full-screen image paints across all 480 rows. | `CM7/TouchGFX/target/TouchGFXHAL.cpp` (flushFrameBuffer wait + EOR ordering) |

### Current verified-working state

- **Branch:** `test/inject-flasher-thumbnail-no-wifi`
- **Latest commit:** `2c60a53` (`feat(pfb): isolated project — strip dispatch fix, all 4 strips paint`)
- **Pushed to:** GitHub (`Ohad324/EWARM-PROJECT-FROM-CubeMX`) + OneDrive `origin` mirror
- **Pipeline proven end-to-end with two test images:**
  - Sysgo Architecture (Porsche 911 GT3, scaled 320×240 stretched to 800×480) — shown today
  - Percepio logo (320×81 letterboxed in 320×240 with white bars, scaled to 800×480) — shown today
- **Test injection path:** one-shot `Music_InjectTestThumb()` at T+3 sec triggers Screen1 → MusicScreen transition → JPEG decode → scale → DMA2D blit → 4-strip DSI transmit. Repeatable.

### Diagnostic infrastructure landed (KEEP for future bugs)

| File / mechanism | Purpose |
|---|---|
| `CM7/Core/Inc/pfb_itm.h` | Bare-metal ITM port-0 helpers (`ITM_PFB(c)`, `ITM_EVENT8(port, value)`, `ITM_EVENT32(port, value)`). No CMSIS dependency. ~30 ns per call. Visible in IAR View → SWO Trace. **Do NOT remove** — invaluable for next bug. |
| `g_pfb_dbg_*` counters | BSS counters for transmitBlock count, mask (which y values seen), allocator state changes, signalVSync ok/fail, waitForVSync entries. Read via IAR Live Watch by name. |
| `EWARM/pfb_dispatch.bat` + `pfb_dispatch_probe.mac` | cspybat one-BP probe at `HAL_DSI_TearingEffectCallback` skip=120, dumps the counters by hardcoded `.map` address. Re-grep map after each rebuild — addresses shift. |
| `EWARM/runs/` | Per-iteration probe logs. Latest: `sysgo_display.log`, `percepio_display.log`. |

### Debug-only state still active (revert before merging to main)

| Item | File | Action |
|---|---|---|
| Red Screen1 background | `CM7/TouchGFX/generated/gui_generated/src/screen1_screen/Screen1ViewBase.cpp` | Already reverted to `RGB(0,0,0)` today |
| Disabled tasks | `CM7/Core/Src/main.c` | Already re-enabled today (videoTask, UARTReceiveTask, CommandHandler, RtosTrace_DrainTask) |
| `Music_InjectTestThumb()` one-shot at T+3s | `CM7/Core/Src/music_display_task.c` | Currently re-enabled. Decide if test injection should ship to main or only stay in DEBUG branch. |
| Test thumbnail JPEG | `CM7/Core/Inc/test_thumb_jpeg.h` | Currently the Percepio logo (320×81 letterboxed, 5,939 bytes). Replace with whatever the production thumbnail should be. |

### REMAINING / OPEN ISSUES

1. **"Missing E" — EOR sometimes absent in SWO trace.** During steady-state captures we sometimes see `aMM!gX` without the trailing `EFF` for some strips. Theory: ITM FIFO overflows when strip dispatch runs back-to-back at full DSI bandwidth. The EOR ISR fires (panel keeps painting) but the `ITM_PFB('E')` write into the stimulus port gets dropped because the FIFO is saturated. To diagnose: enable ITM Local Timestamps + check the IAR SWO Trace overflow-packet count; OR add `ITM_EVENT32(1, DWT_CYCCNT)` at EOR entry to confirm ISR is firing even when 'E' char drops.

2. **Yellow-instead-of-blue color swap (FIXED 2026-05-07).** Visual showed yellow where blue should be. **FIX:** disabled the legacy post-scale R↔B swap loop in `jpeg_decoder.c:246-279` (wrapped `#if 0`). Confirmed visually by user — colors now correct (blue = blue, red = red, white = white). Theory: LTDC `PF=RGB888` in the current PFB-AXI build reads memory as `[R,G,B]` directly, so the swap from the old SDRAM-framebuffer era was actually inverting correct pixels into wrong order. The historical "Confirmed fixes" table entry from 2026-05-03 (post-scale R↔B swap) is now obsolete with the AXI-PFB pipeline.

3. **Aspect ratio distortion when scaling.** 320×240 → 800×480 is 2.5× horizontal × 2× vertical → wide images get squashed vertically. For source images with aspect ≠ 4:3, either (a) letterbox at the JPEG encode step (already doing this for Percepio), (b) scale to native 800×480 JPEG and skip the runtime scale, (c) crop instead of stretch. **Decide policy before next image swap.**

4. **JpegDisplayTask kept enabled even though `Music_Init()` queues are the only required dependency.** When in pure-PFB-debug mode without test injection, JpegDisplayTask spawns and blocks on `xQueueReceive(xMusicQueue, portMAX_DELAY)` with no producers — harmless but worth a follow-up to split `Music_Init()` into queue-creation + task-creation so we can disable just the task during isolation.

### Methodology lessons codified

1. **ITM markers > theory.** Single-character state markers in suspect functions (`a/b/c/d/M/g/X/E/F` here) reveal the bug in seconds where theory took hours. See "Diagnosis methodology" subsection in the PFB section above.
2. **Visual ground truth > register reads.** "All red across 480 rows" answered in 1 sec what register dumps couldn't tell us in an hour.
3. **Address-by-symbol fails for C++ namespace globals in cspybat 9.4.6.1706** — `&touchgfx::g_pfb_dbg_count` errored. Fall back to hardcoded `.map`-extracted addresses with a comment naming the symbol; re-grep after every BSS-shifting build.
4. **`__itm(cycle, port, value)` is a C-SPY debugger-side macro**, NOT a firmware function. Use it inside `.mac` files at BP-hit handlers when firmware instrumentation is too invasive.
5. **`ITM_EVENT8/32` numeric values in the printable-byte range (`'0' + n`)** if you want them visible in the SWO Trace ASCII view. Otherwise use the hex view — bytes 0x00-0x1F look like silence in the text rendering.

### How to resume

1. Read this Save State block first.
2. Check `git log --oneline -5` to confirm last commit is `2c60a53`.
3. Visual: power up the board. Should boot to Screen1 (now black bg) for ~3 sec, then switch to MusicScreen showing the Percepio logo (currently with wrong colors per remaining issue #2).
4. Pick the next problem from "REMAINING / OPEN ISSUES" — start with #2 (yellow-instead-of-blue) since it's actively visible.

---

## SAVE STATE — 2026-05-11 (end-of-day, mic clock restored, gain pending)

### Confirmed FIXED today

**DFSDM mic clock missing on PC2** (PCM stuck at -30518 = Sinc3 on zero PDM input). Two root causes from PFB-era drift:

1. **`MX_SAI4_Init()` was re-enabled** — Path C-PC2 (commit `72ad9fa`) requires SAI4 OFF so PE2 stays high-Z and doesn't fight DFSDM CKOUT (PC2) on the shared mic-CLK net.
2. **FreeRTOS heap was relocated to D2 SRAM1** at `0x30004200`, directly adjacent to `s_DfsdmBuf` at `0x30004000` → DMA contention.

Both reverted to match `bdc7159` (May 1 verified-working) baseline. Scope confirms **2 MHz CKOUT on PC2** restored.

**Commit: `745de5d`** "fix(audio): restore DFSDM mic clock — gate MX_SAI4_Init + heap back to AXI" — pushed to OneDrive + GitHub.

### Current state

- Branch `test/inject-flasher-thumbnail-no-wifi` at `745de5d`
- DFSDM captures real PDM data (raw values vary, channel ID `0x00` = CH0)
- Full pipeline runs end-to-end: SD write OK, UART OK, GCS HTTP 200, STT call
- **`AUDIO_DEBUG_LCD_DISABLED 1`** — temporary bisect aid in `main.c`, revert to 0 before merging

### Open issue blocking STT

**Speech too quiet** — Audio Quality Report:
```
[PCM] peak=215  dc_offset=119  noise_rms=128  speech_rms=122  snr~-6 dB
[PCM] FAIL: speech too quiet -- increase mic_gain or speak closer
```
Voice at ~−43 dB FS. STT correctly returns `no transcript`.

### How to resume (tomorrow)

1. Read this SAVE STATE block.
2. `git log --oneline -3` should show `745de5d` at top.
3. Power up board, press blue button, **speak loudly within 5 cm of mic** (U21 near LCD).
4. **If `[PCM] peak > 5000`** → STT should transcribe → move to Phase 4 (re-enable LCD, commit, push).
5. **If `peak < 1000`** → apply 4x gain in `voice_recorder.c::StoreDmaChunk` (~L750-754). Exact diff in `~/.claude/plans/typed-honking-clover.md` Phase 3.
6. **After STT works**: set `AUDIO_DEBUG_LCD_DISABLED 0`, verify LCD + audio coexist, commit + push.

### Lessons codified

- "We changed memory, look there" → believe the user, not your own analysis.
- Bisect against known-working commit: `git stash` + `git checkout bdc7159` + flash → scope. Single data point unblocks debugging.
- The boot self-check FAILs are false positives (reads CH1 instead of CH0).
- Audio Quality Report (`raw[0..7]`, `peak`, `noise_rms`, `speech_rms`) is the authoritative signal.

---

## SAVE STATE — 2026-05-11 (evening) — UART pipeline restored, STT still pending volume

### What changed today (after morning's DFSDM clock fix)

End-to-end audio → cloud pipeline now reaches **GCS HTTP 200** for the first time since yesterday's heap shuffle. Root cause of the regression and today's fix:

1. **`xRawBleQueue` was dynamically allocated** in `BLE_UART_Init` (`xQueueCreate`). When commit `745de5d` moved the FreeRTOS heap from D2 SRAM1 back to AXI to fix DFSDM clock contention, the queue moved with it and the UART RX path broke silently — NORA's `AUDIO:READY` reply never reached `UARTReceiveTask`. Migrated to `xQueueCreateStatic` + `xSemaphoreCreateMutexStatic`, control blocks and storage pinned with `#pragma location = ".axi_sram"`.
2. **`s_dma_rx_buf` used `__attribute__((section(".dma_buf")))`** but `.dma_buf` had no `.icf` placement — linker silently fell back to default `.bss`. Made deliberate with `#pragma location = ".axi_sram"`.
3. **`.icf` now defines named regions** `.axi_sram` (D1 AXI), `.sram1` (D2 SRAM1), `.sram2` (D2 SRAM2) so future static buffers are deterministically placed.
4. **Hard Rule #1 (Absolute Ban on Dynamic Allocation)** and **Hard Rule #3 (Section Attribution)** added to this file with the project's memory-region map.

A separate **physical issue** masked the SW fix for several hours: the STM32↔NORA UART jumper wire was intermittently disconnected. Every `STREAM2 FAIL` with `last_err=0x00` was the wire, not the code. After reseating the wire, the static-queue firmware ran cleanly to GCS upload.

### Verified successful end-to-end run (REC_010.wav)

```
T+93135  NORA received AUDIO:FILE
T+93135  NORA sent AUDIO:READY → STM32 RX received cleanly
T+95125  96 512 B streamed in ~2 s (~48 KB/s)
T+95465  TLS cert validated
T+98515  GCS upload OK (HTTP 200)
T+101545 STT call returned "no transcript"
```

### Open issue — voice volume below STT threshold

`[PCM] peak=437..684` (noise floor). STT correctly returns nothing. Tomorrow's first move: apply the queued 4× gain change in `voice_recorder.c::StoreDmaChunk`. Exact diff is preserved in `~/.claude/plans/typed-honking-clover.md` Phase 3.

### Debug aids still active (REVERT before merging to main)

| File | Setting | Action |
|---|---|---|
| `CM7/Core/Src/main.c` | `AUDIO_DEBUG_LCD_DISABLED 1` | Set to 0 after STT confirmed |
| `CM7/Core/Src/main.c` | `videoTask` re-enabled but at `osPriorityLow` | Confirm no LCD interference once re-enabled |

### How to resume

1. Read this SAVE STATE block.
2. `git log --oneline -3` — current commit is the static-queue checkpoint pushed today.
3. Power up board, press blue PC13 button. Speak loudly within 5 cm of mic.
4. If `[PCM] peak > 5000` → STT should transcribe. Move to Phase 4 (re-enable LCD, commit).
5. If `peak < 1000` → apply 4× gain in `voice_recorder.c::StoreDmaChunk`. Build, flash, retest.

### Methodology lessons codified

- **Trust memory-related observations first.** Yesterday's heap move + today's queue allocation = same class of bug. CLAUDE.md Hard Rules #1+#3 are the deterrent.
- **Two failures rule still applies** but **wire continuity is a valid hypothesis early** — Bug B (`STREAM2 FAIL last_err=0x00`) had zero hardware errors, which **only** matches a disconnected wire (silence) or a missing ISR (which placement audit ruled out).
- **`#pragma location = ".name"`** is the canonical IAR idiom. GCC `__attribute__((section(".name")))` works as a compat extension but its section name must still be placed in the `.icf` or it silently falls back to default `.bss`.
