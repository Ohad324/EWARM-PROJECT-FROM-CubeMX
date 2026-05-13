# C-SPY Macro: Clock-to-Mic Pipeline + BDMA Diagnostics

## Overview
File: [EWARM/clock_to_mic_bdma.mac](EWARM/clock_to_mic_bdma.mac)

This macro provides comprehensive monitoring of the audio recording pipeline from RCC clock configuration through SAI4/DFSDM/BDMA activation. It captures both **static boot state** and **dynamic DMA activation** with detailed register readouts.

---

## Breakpoint Summary

### BP1: `main.c:226` — System Boot Diagnostics  
**Fires:** Once at startup  
**Purpose:** Baseline verification before any recording  
**Monitors:**
- RCC clock source (D3CCIPR SAI4ASEL—should be 4 for HSI64)
- Power domain status (PWR_D3CR, VOSRDY)
- SAI4 CR1 SAIEN bit (expect 0 at boot, 1 during recording)

### BP2: `voice_recorder.c:771` — Start_Recording_Pipeline Entry  
**Fires:** When blue button pressed → VoiceRecTask notified → recording starts  
**Purpose:** Capture state **just before** BDMA + DFSDM DMA activation  
**Monitors:** All clock, SAI4, BDMA, DFSDM, and DMA1_Stream1 registers in detail

---

## Register Map

### Clock Path (RCC)
| Register | Address | Purpose |
|----------|---------|---------|
| `RCC_CR` | 0x58024400 | Clock status (PLL, HSI ready bits) |
| `RCC_CFGR` | 0x58024410 | System clock select (SWS[5:3]) |
| `RCC_D3CCIPR` | 0x58024854 | **SAI4A kernel clock select** (bits[7:5]=SAI4ASEL, want 4) |

### SAI4 (PE2 PDM Clock Generator)
| Register | Address | Bits | Purpose |
|----------|---------|------|---------|
| `SAI4_CR1` | 0x5800D804 | bit16 | **SAIEN**—must be 1 for PE2 output |
| `SAI4_FRCR` | 0x5800D808 | various | Frame format (FS offset, polarity) |
| `SAI4_SLOTR` | 0x5800D80C | various | Slot count, width |

### DFSDM1 Filter0 (PDM→PCM Decimation)
| Register | Address | Bits | Purpose |
|----------|---------|------|---------|
| `FLTCR1` | 0x40017100 | 0/1/2/18/29 | **DFEN**(enable), **RDMAEN**(DMA), **RSWSTART**(start), **RCONT**(continuous), **FAST** |
| `FLTISR` | 0x40017118 | 0/3 | **REOCF**(end-of-conversion), **ROVRF**(overrun) |
| `CHCFGR1` | 0x40017000 | 7/5:4/2:0 | **CHEN**(channel enable), **SITP**(clock edge), **SPICKSEL**(clock source=01 for PE2) |

### BDMA1 (D3 SAI4 RX FIFO Drain)
| Register | Address | Bits | Purpose |
|----------|---------|------|---------|
| `BDMA1_CCR` | 0x58025004 | bit0 | **EN**—must be 1 for DMA active |
| `BDMA1_CNDTR` | 0x58025008 | — | Remaining count (8 kicks × 1 byte each) |
| `BDMA1_CPAR` | 0x5802500C | — | Peripheral: SAI4_RDR @ 0x5800DA28 |
| `BDMA1_CM0AR` | 0x58025010 | — | Memory buffer: s_sai4KickBuf @ ~0x30004100 |

### DMA1_Stream1 (DFSDM Output → g_DfsdmBuf)
| Register | Address | Bits | Purpose |
|----------|---------|------|---------|
| `DMA1_S1_CR` | 0x40020028 | 0/8/4/3 | **EN**, **CIRC**(circular), **TCIE**(TC int), **HTIE**(HT int) |
| `DMA1_S1_NDTR` | 0x4002002C | — | Remaining: should decrement, NOT stuck at 128 |
| `DMA1_S1_PAR` | 0x40020030 | — | Peripheral: DFSDM FLTRDATAR |
| `DMA1_S1_M0AR` | 0x40020034 | — | Memory dest: s_DfsdmBuf @ 0x30004000 |
| `DMA1_LISR` | 0x4002000C | — | Stream interrupt status |

---

## Expected Values (At Recording Start)

| Register | Expected | Notes |
|----------|----------|-------|
| `RCC_D3CCIPR[7:5]` | `4` | SAI4A = HSI64 (100 = decimal 4) |
| `SAI4_CR1[16]` | `1` | SAIEN enabled |
| `DFSDM_FLTCR1[2:0]` | `0b111` | RSWSTART=1, RDMAEN=1, DFEN=1 |
| `BDMA1_CCR[0]` | `1` | BDMA DMA transfer active |
| `DMA1_S1_CR[0]` | `1` | DMA1_Stream1 active |
| `DMA1_S1_NDTR` | `<128` | Counting down, not stalled |
| **PE2 (oscilloscope)** | **2.0 MHz** | Continuous square wave for 3+ seconds |

---

## Troubleshooting Flow

### ❌ PE2 Has No Clock After BP2
**Step 1:** Check SAI4_CR1 SAIEN bit
- If SAIEN = 0 → SAI4 never enabled → review HAL_SAI_Receive_DMA call
- If SAIEN = 1 → Check BDMA activity

**Step 2:** Check BDMA1_CCR.EN and CNDTR
- If EN = 0 → BDMA didn't start → check HAL_SAI_Receive_DMA return code
- If EN = 1 but CNDTR stuck → BDMA stalled (FIFO full, no drain)

**Step 3:** Check DMA1_S1_CR.EN and NDTR
- If EN = 0 → DFSDM DMA didn't start → check HAL_DFSDM_FilterRegularStart_DMA return
- If EN = 1 but NDTR = 128 (unchanged) → DFSDM not producing data

### ❌ PE2 Has Clock But No PCM Data (g_DfsdmBuf = zeros)
**Check:** DFSDM_CHCFGR1 SPICKSEL + SITP bits
- SPICKSEL[2:0] = 1 (CKOUT/PE2 selected)
- SITP[5:4] = 1 (falling edge)
- If wrong → DFSDM sampling on wrong clock edge or wrong source

**Check:** DFSDM_FLTISR ROVRF bit
- If ROVRF = 1 → overrun (DMA too slow or OSR wrong)

### ❌ DMA Moving But NDTR Stuck at Specific Value
**Possible causes:**
- DMA not circular (CIRC bit = 0)
- Half/full-complete interrupts not firing
- IRQ priority too low → callback blocked by higher-priority task

---

## Usage Instructions

1. **Load the macro in IAR Embedded Workbench:**
   - Debug → Macros → Execute Macro
   - Select [EWARM/clock_to_mic_bdma.mac](EWARM/clock_to_mic_bdma.mac)

2. **At BP1 (main entry):**
   - Review RCC clock config (D3CCIPR should show HSI64 selected)
   - Note PWR_VOSRDY status
   - Press ▶ (continue) to run until next breakpoint

3. **At BP2 (Start_Recording_Pipeline):**
   - Press blue button on board (PC13) or trigger in simulator
   - Macro captures **PRE-ACTIVATION state** (all DMAs at 0)
   - **CRITICAL:** After stepping past HAL calls:
     - Check BDMA1_CCR.EN changed to 1
     - Check DMA1_S1_CR.EN changed to 1
     - Scope PE2: should see 2.0 MHz within ~1 µs

4. **During Recording (3 seconds):**
   - DMA1_S1_NDTR continuously decrements
   - DFSDM_FLTISR ROVRF must stay 0
   - g_DfsdmBuf at 0x30004000 fills with PCM samples

5. **After Recording:**
   - Check g_AudioBuf @ 0x30020000 for final PCM data
   - WAV file on SD card (REC_XXX.wav)

---

## Key Design Insight: BDMA is Critical

**Why BDMA?**  
SAI4 is in D3 power domain (low-power always-on). When SAIEN=1 without DMA:
- SAI4 RX FIFO fills in ~1 µs
- Hardware **kills PE2 clock** to save power
- Mic stops sampling
- **BDMA must drain FIFO continuously** via 8-byte dummy transfers

**BDMA before DFSDM DMA:**  
Order matters:
1. HAL_SAI_Receive_DMA() → BDMA starts, PE2 runs
2. HAL_DFSDM_FilterRegularStart_DMA() → DFSDM sees real 2 MHz clock

If reversed or skipped → PE2 never outputs → silence.

---

## References

- **STM32H7 RM:** Section 38 (SAI), 53 (DFSDM), 42 (RCC)
- **Voice Recorder Code:** [CM7/Core/Src/voice_recorder.c](CM7/Core/Src/voice_recorder.c)
- **Related Macros:**
  - [dfsdm_debug.mac](EWARM/dfsdm_debug.mac) — 6 breakpoints for DFSDM data flow
  - [dfsdm_watch_all.mac](EWARM/dfsdm_watch_all.mac) — SPICKSEL switching + full pipeline
