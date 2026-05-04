# CLAUDE.md — STM32H747I-DISCO Voice Recorder

## Project Identity
- Board: STM32H747I-DISCO
- Active core: Cortex-M7
- Toolchain: CubeMX + IAR
- IAR project: `EWARM/STM32H747I-DISCO.ewp`
- Active target: `STM32H747I-DISCO_CM7`

## Documentation Rule — MANDATORY, NO EXCEPTIONS

Before proposing or implementing ANY fix (trivial or not), you MUST read all 3 of these sources in order:

1. **User Manual / Application Note** — ST UM or AN relevant to the peripheral (e.g. UM2411, AN5027)
2. **Component Datasheet** — the exact part number on the board (e.g. MP34DT05-A, not a generic)
3. **Community** — ST Community forum, or a colleague/Gemini cross-check for known issues

**Do not write a single line of code until all 3 have been consulted.**

Reason: ST has thousands of developers, excellent docs, and an active community. The answer almost always exists. Guessing wastes hours. Reading saves them.

- Always check `docs/` folder first before asking the user to find a document.
- If a document is missing from `docs/`, tell the user which specific document is needed and why.

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

9. No dynamic allocation.  
   No `malloc`, `pvPortMalloc`, `new`.  
   Use static or global buffers only.

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

## C-SPY Macro Reference (UCSARM-26, v9.50.x)

Authoritative reference source: **IAR C-SPY Debugging Guide for Arm (UCSARM-26), version 9.50.x.** Cross-checked against `docs/EWARM_DebuggingGuide.ENU.pdf` (in this repo). The older `docs/CSPYBAT_AND_CSPY_MACRO_GUIDE.md` mentions `__go()` / `__stop()` — **those are NOT in the current IAR macro language**. Do not use them. Use `__hwRunToBreakpoint`, `__delay`, and the proper reserved hooks listed below.

### Reserved Setup Macros (auto-fired by debugger)

| Macro | When it runs |
|---|---|
| `execConfigureTraceETM` | When configuring ETM trace |
| `execConfigureTraceSWO` | When configuring SWO trace |
| **`execUserAttach`** | **When attaching to a running target** ← correct hook for `--attach_to_running_target` |
| `execUserPreload` | Before the application is loaded |
| `execUserExecutionStarted` | Each time execution starts |
| `execUserExecutionStopped` | Each time execution stops (BP hit, halt request, etc.) |
| `execUserFlashInit` | Before flash programming |
| **`execUserSetup`** | **Once, after the application is loaded** ← only fires on download flow, NOT on attach |
| `execUserFlashReset` | After flash reset |
| `execUserPreReset` | Before a target reset |
| `execUserReset` | After a target reset |
| `execUserExit` | When the debug session ends |
| `execUserFlashExit` | After flash programming |
| `execUserCoreConnect` | When connecting to a core (multicore) |

**Critical: `execUserSetup` does NOT fire in `--attach_to_running_target` mode** — use `execUserAttach` instead. Confirmed empirically against this project: `boot_pipeline.mac` was silent under attach until renamed.

### System Macros (called from your code)

**Breakpoints:**
`__setCodeBreak`, `__setDataBreak`, `__setDataLogBreak`, `__setLogBreak`, `__setSimBreak`, `__setTraceStartBreak`, `__setTraceStopBreak`, `__clearBreak`, `__restoreSoftwareBreakpoints`

**Memory access:**
`__readMemory8`/`Byte`, `__readMemory16`, `__readMemory32`, `__readMemory64`, `__writeMemory8`/`Byte`/`16`/`32`/`64`, `__fillMemory8`/`16`/`32`/`64`, `__memorySave`, `__memoryRestore`

**Execution control (NO `__go` / `__stop`):**
`__delay(ms)`, `__hwReset`, `__hwResetWithStrategy`, `__hwResetRunToBp`, `__hwJetResetWithStrategy`, **`__hwRunToBreakpoint`**, `__abortLaunch`

**Interrupts (mostly simulator):**
`__enableInterrupts`, `__disableInterrupts`, `__orderInterrupt`, `__cancelInterrupt`, `__cancelAllInterrupts`, `__popSimulatorInterruptExecutingStack`

**Files (host PC):**
`__openFile`, `__closeFile`, `__readFile`/`Byte`, `__writeFile`/`Byte`, `__resetFile`

**Image / symbol management:**
`__loadImage`, `__unloadImage`, `__registerMacroFile`, `__sourcePosition`, `__evaluate`, `__expandVar`, `__isMacroSymbolDefined`, `__symbolAddress`

**String operations:**
`__strFind`, `__subString`, `__toLower`, `__toUpper`, `__toString`

**Probe and debug interface:**
`__driverType`, `__probeType`, `__targetDebuggerVersion`, `__emulatorSpeed`, `__emulatorStatusCheckOnRead`, `__getTracePortSize`, `__hasDAPRegs`, `__readAPReg`/`writeAPReg`, `__readDPReg`/`writeDPReg`

**JTAG-level access:**
`__jtagCommand`, `__jtagData`, `__jtagRawRead`/`Write`/`Sync`, `__jtagResetTRST`, `__jtagCP15IsPresent`, `__jtagCP15ReadReg`/`WriteReg`

**Probe-specific commands:**
`__jlinkExecCommand`, `__jlinkExecMacro`, `__gdbserver_exec_command`

**Multicore:**
`__getNumberOfCores`, `__getSelectedCore`, `__selectCore`

**Output and user interaction:**
`__message` (statement, not function), `__messageBoxYesNo`, `__messageBoxYesCancel`

**System / shell:**
`__system1`, `__system2`, `__system3`

**Miscellaneous:**
`__isBatchMode` (true under cspybat), `__wallTime_ms`

### Macro language quirks (project-discovered)

- **No preprocessor.** No `#define`, no `#include`, no `#ifdef`. Inline literal addresses; use `__var` locals only.
- **No structs / enums.** Flat scalars only.
- **Format specifiers.** Use `:%08X` for hex, `:%d` for decimal — appear inside `__message` argument lists.
- **`__var`** declares a local; assignments don't need a type. Numbers are 32-bit unsigned typically.
- **Boolean: `if (a == b)` works, `&&` and `||` work.** No `!=`-with-bitwise on the same line without parens.
- **`__readMemory32(addr, "Memory")`** — the second argument is the zone name (always `"Memory"` for normal SoC peripheral / RAM access).

### cspybat invocation patterns observed in this project

| Use case | Flags | Hook |
|---|---|---|
| Download + run + sample at fixed BP | (default flow) | `execUserSetup` |
| Attach to already-running chip, sample, leave running | `--attach_to_running_target --leave_target_running` | **`execUserAttach`** |
| Flash only | `--download_only --silent` | (none) |
| Suppress macro `__message` output | `--silent` | — (do not use if you want the report) |

Single source of truth for exact expected output: see `EWARM/boot_pipeline_cspy.bat` + `EWARM/boot_pipeline.mac`.

### Debug Session Lifecycle (download flow)

The reserved hooks fire in this order during a typical download-and-debug session:

```
1.  cspybat starts
        |
        v
2.  Debugger connects to target via probe
        |
        v
3.  execUserPreload()        <- hardware prep BEFORE firmware is loaded
        |
        v
4.  Firmware downloaded to flash/RAM
        |
        v
5.  execUserSetup()          <- install breakpoints, resolve symbols
        |
        v
6.  CPU starts running
        |
        v
   +--------------------------------------------+
   | Loop:                                      |
   |   execUserExecutionStarted()  <- CPU runs  |
   |   ... target runs ...                      |
   |   execUserExecutionStopped()  <- BP hit /  |
   |                                  halt      |
   |   (your BP action macros fire here)        |
   +--------------------------------------------+
        |
        v   (if a reset happens at any point)
7.  execUserPreReset()       <- BEFORE the reset signal
        |
        v
   [hardware reset occurs]
        |
        v
8.  execUserReset()          <- AFTER the reset
        |
        v
9.  execUserExit()           <- session ending, clean up
        |
        v
10. cspybat exits
```

For attach mode (`--attach_to_running_target`), the equivalent lifecycle is shorter: connect → `execUserAttach()` (hardware is already running) → optionally halt → samples / inspections → `execUserExit()`. The download/preload/reset hooks are NOT fired.

#### Hook usage notes

- **`execUserPreload`** — hardware prep before download. Use to enable external SDRAM/QSPI when linker places code there.
- **`execUserSetup`** — workhorse. Runs once after firmware load, CPU paused at reset vector. Install breakpoints, resolve `__symbolAddress`, open log files.
- **`execUserExecutionStarted`** — every CPU resume. Rare uses (timing host-side wall clock between halts).
- **`execUserExecutionStopped`** — every halt (BP hit, single-step, manual halt). **Do not put heavy work here** — runs on every step too.
- **`execUserPreReset` / `execUserReset`** — bracket a reset. Save/restore state, mark log boundaries.
- **`execUserExit`** — teardown. Clear breakpoints, close files, final banner.

#### BP-driven probe pattern (recommended over `__delay` timing)

```c
// In execUserSetup, install BPs at peripheral-init exits:
__var bp_dsi, bp_ltdc, bp_tgfx;

execUserSetup()
{
    bp_dsi  = __setCodeBreak(__symbolAddress("MX_DSIHOST_DSI_Init") + ..., 0, "1", "TRUE", "");
    bp_ltdc = __setCodeBreak(__symbolAddress("MX_LTDC_Init") + ...,        0, "1", "TRUE", "");
    bp_tgfx = __setCodeBreak(__symbolAddress("MX_TouchGFX_Init") + ...,    0, "1", "TRUE", "");
}

execUserExecutionStopped()
{
    __var pc;
    pc = __readMemory32(0xE000EDF8, "Memory");   // DCRDR (or use __evaluate("$PC$"))
    // Dispatch by which BP hit, snapshot peripheral state, clear that BP, continue.
}
```

This is more precise than `__delay()` timing because it fires at exact transitions rather than arbitrary wall-clock points.

#### Mental model: hooks as "anything to do here?" prompts

Every C-SPY debug session follows the same built-in routine. The hooks are simply "anything you want to do here?" prompts the debugger asks you at each step:

```
Connect to the target
   [hook: execUserPreload          -- anything to do here?]
Download firmware
   [hook: execUserSetup            -- anything to do here?]
Run the CPU
   [hook: execUserExecutionStopped -- anything to do here when it halts?]
Eventually end the session
   [hook: execUserExit             -- anything to clean up?]
```

If you don't define a hook, the debugger just continues with its built-in routine — empty hooks are fine. You only define the ones where you actually have work to do.
