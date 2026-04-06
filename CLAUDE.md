# CLAUDE.md — STM32H747I-DISCO Voice Recorder

## Project Identity
- Board: STM32H747I-DISCO
- Active core: Cortex-M7
- Toolchain: CubeMX + IAR
- IAR project: `EWARM/STM32H747I-DISCO.ewp`
- Active target: `STM32H747I-DISCO_CM7`

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
| **PDM decimation setting** | `PDM_FILTER_DEC_FACTOR_64` (OSR = 64) |
| **Required PDM clock** | 16,000 × 64 = 1.024 MHz |
| **Actual SAI4 mic clock** | PLL2P (49.14 MHz) / (MCKDIV=24 × 2) = **1.02381 MHz** (0.02% error — negligible) |

PLL2 config used in this project:
- HSE=25 MHz / PLL2M=25 → VCO_in=1 MHz × PLL2N=344 / PLL2P=7 → **49.14 MHz** (SAI4A kernel clock)
- HAL computes MCKDIV=24 → PDM clock = 49.14 MHz / 48 ≈ **1.02381 MHz**
- `AudioFrequency = SAMPLE_RATE × 8 = 128000` (HAL uses this to derive MCKDIV)

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
6. LR=GND → mic outputs on falling edge → `SAI_CLOCKSTROBING_FALLINGEDGE` ✅

### 6. Memory Placement — D3 SRAM for BDMA

SAI4 is in the D3 domain. Its DMA controller is **BDMA**, which can **only access D3 SRAM**.

```c
#pragma location = 0x38000000          // D3 SRAM — only region BDMA can reach
static __no_init uint16_t g_PdmBuf[PDM_BUF_TOTAL];
```

PCM output buffer (`g_AudioBuf`) lives in AXI SRAM (D1, default `.bss`) — written by the CPU task, not BDMA, so this is correct.

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

