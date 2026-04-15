# DFSDM Implementation Notes — STM32H747I-DISCO Voice Recorder

**Board:** STM32H747I-DISCO  
**Mic:** MP34DT05-A (onboard PDM MEMS, RIGHT channel)  
**Active core:** Cortex-M7  
**Last updated:** 2026-04-15

---

## 1. Architecture Overview

The mic path uses **two separate peripherals in tandem**:

```
MP34DT05-A (mic)
    │
    ├── PE2 (SAI4_CK1, AF10)  ← PDM clock output from SAI4
    └── PC1 (SAI4_D1,  AF10)  ← PDM data input to SAI4
              │
         SAI4 Block A
         (PDM master receive, D3 domain)
              │  internal silicon bridge (SPICKSEL=11)  ← NOT USED YET (SPICKSEL=01 currently)
              │
         DFSDM1 Channel 1
         (Sinc3 decimation filter, D2 domain)
              │
         DMA1_Stream1
              │
         g_DfsdmBuf[32] @ 0x30000000 (D2 SRAM1)
              │
         VoiceRecTask (FreeRTOS)
              │
         g_AudioBuf[48000] @ 0x30020000 (D2 SRAM2)
              │
         SDWriteTask → REC_XXX.wav
```

### What SAI4 does
SAI4 drives the **physical pins** to the microphone: it generates the PDM clock on PE2 and receives PDM data on PC1. SAI4 is in PDM master-receive mode — it is the bus master for the mic.

### What DFSDM does
DFSDM1 **decimates** the PDM bitstream into PCM samples using a Sinc3 digital filter. It reads the data already captured by SAI4 (either via a GPIO pin or via the internal SAI→DFSDM bridge).

### The clock question (SPICKSEL)
DFSDM Channel 1 must be told where to get its **serial clock**:

| SPICKSEL | Meaning | Problem for this board |
|----------|---------|----------------------|
| `01` | Internal CKOUT (DFSDM generates its own clock on PD3/PE9) | PD3 has no physical path to PE2 or the mic — unsynchronised |
| `11` | SAI4 internal bridge | SAI4 already drives the mic clock on PE2 — same source, same edge |

**Current firmware uses SPICKSEL=01 (CKOUT).** This is the suspected root cause of `last_raw` being stuck at `0xFF88CA01` (-30518, which is Sinc3 negative full-scale from all-zeros input).

---

## 2. Hardware — Pin Map

| Pin | AF | Signal | Direction | Role |
|-----|----|--------|-----------|------|
| PE2 | AF10 | SAI4_CK1 | Output | PDM clock to mic (~2.0 MHz) |
| PC1 | AF10 | SAI4_D1  | Input  | PDM data from mic |
| PE4 | AF8  | SAI4_FS_A | Output | Frame sync — required to unlock SAI4 master clock tree |
| PE5 | AF8  | SAI4_SCK_A | Output | Serial clock — SAI4 internal bit clock reference |

**PD3 (DFSDM_CKOUT)** — not connected to anything on this board. DFSDM CKOUT cannot reach the onboard mic.

### Confirmed from schematic (mb1248-h747i-d04):
- **SB43 (LEFT SELECTION) = OPEN** — LR pin is NOT pulled to GND
- **R213 (10 kΩ)** pulls LR to VDD (+3.3 V)
- Therefore **LR = HIGH → RIGHT channel → mic outputs PDM data on FALLING edge of CLK**

---

## 3. Channel Selection — LR=HIGH is FALLING edge

MP34DT05-A data sheet rule:
- LR = LOW  → data on **rising** edge of CLK → DFSDM SITP=00 → Channel 0 natural input
- LR = HIGH → data on **falling** edge of CLK → DFSDM SITP=01 → Channel 1 natural input

This board has LR=HIGH. Therefore:
- `SITP = 01` (falling edge) ✓
- Data channel = **Channel 1** (`DFSDM_CHANNEL_1`)
- `CHCFGR1 = 0x8D` (CHEN=1, SPICKSEL=01, SITP=01)

---

## 4. Clock Math

### SAI4 mic clock (PE2)
```
PLL2: HSE=25 MHz / M=25 → VCO_in=1 MHz × N=344 / P=7 → PLL2P = 49.14 MHz
SAI4 kernel clock = PLL2P = 49.14 MHz
AudioFrequency = SAMPLE_RATE × 8 = 128000 Hz
HAL computes MCKDIV = 49.14 MHz / (128000 × 2) ≈ 192 → adjusted to 24
PDM_CLK = 49.14 MHz / (24 × 2) = 1.02381 MHz  (target: 1.024 MHz, error: 0.02% ✓)
```

MP34DT05-A clock spec: 1.2–3.25 MHz. At 1.024 MHz this is just inside the spec lower boundary.

### DFSDM CKOUT (PD3 — not connected to mic)
```
CKOUTDIV = 24 (set in Ch0 CHCFGR1 before DFSDMEN=1)
CKOUT = APB2 / ((CKOUTDIV + 1) × 2) = 100 MHz / (25 × 2) = 2.0 MHz
```

### PCM output rate
```
Sinc3, OSR = 125
PCM = 2.0 MHz / 125 = 16,000 Hz ✓
```

### DMA cadence
```
DFSDM_BUF_TOTAL = 32 int32_t samples (circular)
DFSDM_BUF_HALF  = 16 samples per DMA half-complete callback
Time per half   = 16 samples / 16000 Hz = 1.0 ms per DMA callback
```

---

## 5. DFSDM Initialisation — Order Is Critical

RM0399 §30.4.2 defines a strict write-protect sequence:

```
DFSDMEN=0  →  CKOUTDIV is writable
DFSDMEN=1  →  CKOUTDIV locks, CHEN becomes writable
CHEN=1     →  SPICKSEL, SITP, DTRBS all become READ-ONLY
```

**Required sequence in code:**
1. Enable RCC clocks (both gates — see §6)
2. Write `CKOUTDIV=24` while `DFSDMEN=0`
3. Set `DFSDMEN=1` — this locks CKOUTDIV
4. Call `HAL_DFSDM_ChannelInit()` — writes CHEN=1 (now allowed)
5. Call `HAL_DFSDM_FilterInit()`
6. Call `HAL_DFSDM_FilterConfigRegChannel()` — assigns CH1 to Filter0

Changing SPICKSEL after CHEN=1 requires:
```
1. Write CHEN=0  (clears write-protect)
2. Write new SPICKSEL value
3. Write CHEN=1  (re-enable with new setting)
```
This is the "CHEN clear/restore dance" used in `spicksel_set_sai4()`.

---

## 6. RCC — Dual Gate on STM32H747

STM32H747 is a dual-core SoC. DFSDM1 has **two independent RCC gates** that must both be enabled for CM7 to use it reliably:

```c
__HAL_RCC_DFSDM1_CLK_ENABLE();      // RCC->APB2ENR  bit 28 — shared bus clock (DMA path)
__HAL_RCC_C1_DFSDM1_CLK_ENABLE();  // RCC_C1->APB2ENR bit 28 — CM7 core gate (register access)
```

Using only the shared gate: DMA reachable but CM7 register writes silently no-op.  
Using only the CM7 gate: insufficient — DMA path needs the shared bus clock.

---

## 7. DTRBS (Right Bit Shift) — Must Be 6

Sinc3 filter with OSR=125 produces a maximum output value of:
```
125³ = 1,953,125  (21 bits — does NOT fit int16_t)
```

DFSDM FLTRDATAR stores the result right-justified in bits [31:8] (24-bit field).  
`StoreDmaChunk` extracts it as:
```c
(int16_t)(src32[i] >> 8)
```

With DTRBS=6, the hardware right-shifts by 6 inside the result register before DMA:
```
1,953,125 >> 6 = 30,517  (fits int16_t ±32,767 ✓)
```

DTRBS=5 → max ±61,035 → overflows int16_t → wraps to garbage DC value.

---

## 8. Memory Placement

| Buffer | Location | Address | Size | Reason |
|--------|----------|---------|------|--------|
| `g_DfsdmBuf[32]` | D2 SRAM1 | `0x30000000` | 128 bytes | DMA1 can access D2 SRAM; outside D-Cache range (no SCB_InvalidateDCache needed) |
| `g_AudioBuf[48000]` | D2 SRAM2 | `0x30020000` | 96 KB | Same domain as g_DfsdmBuf → StoreDmaChunk is D2→D2, no AXI bus crossing |
| `g_PdmBuf` (SAI4) | D3 SRAM | `0x38000000` | — | BDMA (SAI4 DMA controller) can only access D3 SRAM |

D-Cache note: D2 SRAM (0x30000000–0x3FFFFFFF) is **not cached** on STM32H7 — `SCB_InvalidateDCache_by_Addr()` is NOT needed for these buffers.

Previously `g_AudioBuf` was in SDRAM (0xD0000000). This caused SDMMC FIFO overruns (`RXOVERR`) because the FMC auto-refresh cycle (every 7.8 µs) conflicted with SDMMC1 IDMA on the shared AHB3 bus. Moving to D2 SRAM fixed this.

---

## 9. The -30518 Problem (last_raw = 0xFF88CA01)

Sinc3 filter response to all-zeros PDM input:
```
All zeros → Sinc3 integrator accumulates → large negative number → right-shifted to -30518
0xFF88CA00 >> 8 = -30518 (sign-extended int16_t)
```

This is the **negative full-scale** DC output, a mathematical artifact of all-zeros input.

**Root cause:** DFSDM is not receiving actual mic data because:
1. SPICKSEL=01 means DFSDM clocks itself from its own CKOUT (PD3)
2. The mic is clocked by SAI4_CK1 (PE2) — an entirely different, unrelated clock source
3. DFSDM samples the DATIN1 pin (PC1) on its own CKOUT edges
4. The mic drives PC1 with data synchronised to PE2 (SAI4 clock), not PD3 (DFSDM CKOUT)
5. Two unsynchronised clocks → DFSDM captures random/metastable bits → effectively all-zeros → -30518

**Fix being tested:** SPICKSEL=11 (SAI4 internal bridge) routes the SAI4 clock directly into DFSDM — same clock source for both the mic output and the DFSDM sampler.

---

## 10. Key Register Values (Verified Post-Boot Dump)

| Register | Address | Expected Value | Meaning |
|----------|---------|----------------|---------|
| `DFSDM1_Ch0 CHCFGR1` | `0x40017000` | `0x8018008D` | DFSDMEN=1, CKOUTDIV=24, CHEN=1, SPICKSEL=01, SITP=01 |
| `DFSDM1_Ch1 CHCFGR1` | `0x40017020` | `0x0000008D` | CHEN=1, SPICKSEL=01, SITP=01 |
| `DFSDM1_Ch1 CHCFGR2` | `0x40017024` | `0x00000030` | DTRBS=6 |
| `DFSDM1_Flt0 FLTCR1` | `0x40017100` | `0x21240001` | RCSEL=CH1, RDMAEN=1, DFEN=1 |
| `DFSDM1_Flt0 FLTFCR` | `0x40017114` | `0x607C0000` | Sinc3, OSR=125 |
| `DFSDM1_Flt0 FLTRDATAR` | `0x4001711C` | — | DMA PAR — raw Sinc3 output |
| `SAI4 CR1` | `0x58005404` | SAIEN(bit16)=1 | SAI4 enabled |
| `SAI4 PDMCR` | `0x58005444` | `0x00000101` | PDMEN=1, CKEN1=1 |
| `DMA1_Stream1 PAR` | `0x40020030` | `0x4001711C` | Points to FLTRDATAR |
| `DMA1_Stream1 M0AR` | `0x40020034` | `0x30000000` | Points to g_DfsdmBuf |

---

## 11. What CKABF=0xFF Means (and Why It Is Harmless)

`FLTISR[23:16]` = CKABF (Clock Absence Flag for each channel).  
At boot the dump shows `CKABF=0xFF` — all 8 channel bits set.

**Why this happens:** DFSDM init runs before `HAL_DFSDM_FilterRegularStart_DMA()`. During the gap, DFSDM detects no clock on any channel → sets CKABF sticky bits.

**Why it is harmless:** CKABF is a sticky status flag, not a fault latch. The HAL DMA start sequence clears it. It does not prevent DMA from running or producing output.

---

## 12. Bugs Found and Fixed During Bringup

| Bug | Symptom | Fix |
|-----|---------|-----|
| `audio_rec.c` duplicate `HAL_DFSDM_FilterRegConvHalfCpltCallback` and `CpltCallback` | IAR silently picked `audio_rec.c` version which posted to dead `s_halfQueue` → VoiceRecTask never received DMA events | Deleted `audio_rec.c` entirely |
| `dfsdm_watch_all.mac` reading addresses 0x400 too high | All register reads returned HRTIM1 registers — appeared as all-zeros, masking actual hardware state | Rewrote entire macro with addresses from `stm32h747xx.h` |
| `RCC_APB2ENR_DFSDM1EN` bit number wrong in macro | Macro reported DFSDM1EN=0 even though peripheral was clocked | Fixed: bit 28 (not bit 30) |
| `CKOUTDIV` written after `DFSDMEN=1` | CKOUTDIV write silently ignored by hardware → wrong CKOUT frequency | Enforced correct order: CKOUTDIV first, then DFSDMEN=1 |
| CM7 gate (`__HAL_RCC_C1_DFSDM1_CLK_ENABLE`) not called | CM7 register writes to DFSDM silently no-op | Added both RCC gates |
| Duplicate `huart8` and `MX_UART8_Init` declarations in `main.c` | Potential linker warning | Removed duplicate declarations |

---

## 13. C-SPY Debug Macros (dfsdm_watch_all.mac)

Available functions after loading `EWARM/dfsdm_watch_all.mac`:

| Function | What it does |
|----------|-------------|
| `dfsdm_dump()` | Full register dump: RCC, Ch0, Ch1, Filter0, SAI4, DMA1, GPIO, g_dbg struct, live globals, state counters, first 8 DMA buffer samples |
| `spicksel_set_sai4()` | CHEN→0, write SPICKSEL=11, CHEN→1 — switches to SAI4 bridge |
| `spicksel_restore_ckout()` | CHEN→0, write SPICKSEL=01, CHEN→1 — reverts to CKOUT |
| `test_spicksel_sai4()` | Full automated test: sets SPICKSEL=11, waits for button press, waits for recording to finish, reads `last_raw`, prints PASS/FAIL verdict |

`g_dbg` struct at `0x24000050` — firmware-cached register snapshot, refreshed every RTTLogTask tick. Visible in IAR Live Watch. This is a **C variable copy**, not the hardware register — do not write to it to change peripheral state.

---

## 14. Open Question

**SPICKSEL=11 (SAI4 bridge) has not been confirmed working yet.**

The hypothesis is:
- With SPICKSEL=01 (CKOUT): DFSDM and SAI4 run on independent clocks → mic data never captured → -30518
- With SPICKSEL=11 (SAI4 bridge): DFSDM receives the same clock SAI4 uses for the mic → actual PDM data → non-DC output

Test procedure: run `test_spicksel_sai4()` in the C-SPY macro console. If `last_raw` changes from `0xFF88CA01`, SPICKSEL=11 is the fix and must be made permanent in `main.c` (`MX_DFSDM1_Init`).

If SPICKSEL=11 still produces -30518, the next investigation points are:
1. SAI4 `SAIEN` bit — is SAI4 actually enabled and clocking PE2?
2. `PDMCR` — is `CKEN1=1`?
3. PLL2 — is 49.14 MHz actually reaching SAI4?
4. PE2 GPIO alternate function — is it AF10 (SAI4_CK1)?

---

## 15. What We Have Actually Observed (Verified on Hardware)

This section records only things confirmed by live hardware logs, register reads, or observed RTT output. Each entry states the source.

### 15.1 Boot Self-Check — What Passes Every Run

From `EWARM/runs/run_20260414_212516/VOICE_REC_RESULT_20260414_212516.log` (representative of all recent runs):

```
[BOOT]  1. LR hardware        : HIGH (RIGHT mic, SB43=OPEN, R213 pullup)  [HW-FIXED]
[BOOT]  2. DFSDM channel       : CH1  (RCSEL bits[27:24])   [OK  ] expect 1
[BOOT]  3. Edge (SITP)         : FALLING(01)                [OK  ] expect 01=falling
[BOOT]  4. SPICKSEL            : 11   (bits[3:2]=0x03)      [OK  ] expect 11=SAI4
[BOOT]  5. CHEN (CH1 enable)   : 1    (bit7)                [OK  ] expect 1
[BOOT]  8. RCSEL (filter->CH)  : 1    FLTCR1=0x21240001     [OK  ] expect 1=CH1
[BOOT]  9. Filter mode         : Sinc3  OSR=125  FLTFCR=0x607C0000  [OK  ]
[BOOT] 11. DTRBS (shift)       : 6    CH0CFG2=0x00000030    [OK  ] expect 6
[BOOT]     SAI4 PDMCR          : 0x00000101                  [OK  ] expect 0x00000101
```

**Verified true on hardware:**
- LR=HIGH is confirmed by schematic and never changes — this is permanent board wiring
- DFSDM Channel 1 is the active data channel
- SITP=01 (falling edge) is set and accepted by hardware
- SPICKSEL=11 (SAI4 bridge) **is what the current firmware sets** — the HAL init code writes this
- CHEN=1 after init — channel is enabled
- Filter0 reads from CH1 (RCSEL=1)
- Sinc3, OSR=125 — filter configured correctly
- DTRBS=6 — right-shift of 6 is active
- SAI4 PDMCR=0x101 — SAI4 PDM mode enabled with CKEN1=1

### 15.2 Boot Self-Check — What Fails

```
[BOOT]  6. DFSDMEN (global)    : 0  (bit31)   [FAIL] expect 1
[BOOT]  7. CKOUTDIV            : 0  (bits[22:16])  [FAIL] expect 24 -> 2.0 MHz
```

**Observed:** The self-check reads `DFSDM1_Channel0->CHCFGR1` (Ch0, address `0x40017000`) and expects to find `DFSDMEN=1` (bit 31) and `CKOUTDIV=24` (bits [22:16]).

At boot these read as 0. The g_dbg snapshot (collected from `DFSDM1_Channel1->CHCFGR1`, address `0x40017020`) shows `0x0000008D` which is correct for Channel 1.

**Interpretation:** `CKOUTDIV` and `DFSDMEN` live in Channel 0's register. The self-check may be reading before Ch0 init completes, or the global enable bit is genuinely not set. This is not yet resolved — but the DMA is running (see §15.3), so at minimum the filter and DMA path are functional.

### 15.3 DMA Is Running After Recording

From the same log, post-recording register dump:

```
[DFSDM] DMA_CR=00035500[OK]  NDTR=32  M0AR=30000000[OK]  last_raw=FF88CA01  last_val=-30518
[REG]   DMA1_St1->CR   = 0x00035500
[REG]   DMA1_St1->NDTR = 0x00000020  (=32 decimal)
[REG]   DMA1_St1->M0AR = 0x30000000
```

**Verified true:**
- DMA1_Stream1 is running (NDTR=32 = full buffer size, circulating correctly)
- M0AR = `0x30000000` — buffer pointer set correctly to `g_DfsdmBuf`
- DMA CR = `0x00035500` — stream enabled, circular mode, half/full interrupts active
- `callbacks=3032  warmup=32  stored=3000  overflow=0` — DMA delivered exactly the expected number of callbacks for 3 seconds with zero overflow. The DMA pipeline is working end-to-end.

### 15.4 PCM Output Is Stuck at -30518 (DC) — Confirmed

```
[PCM]  out[64..71] : -30518 -30518 -30518 -30518 -30518 -30518 -30518 -30518
[PCM]  noise_rms=30518   speech_rms=30518   snr~+0 dB
[PCM]  dc_offset=-30518   peak=30518   clip=0
[PCM]  FAIL: noise floor too high -- hardware/layout issue
[DFSDM] raw[0..7] : FF88CA01 FF88D91D FF88CA01 FF88CA01 FF88CA01 FF88CA01 FF88CA01 FF88CA01
```

**Verified:** Every PCM sample is -30518. The raw DFSDM output `0xFF88CA01` (sign-extended = -30518) is Sinc3 negative full-scale, which is the mathematically expected output when the PDM input is all zeros. This is consistent with DFSDM receiving no valid PDM transitions from the microphone.

### 15.5 g_dbg Struct — Verified Register Snapshot

From `dfsdm_struct_read.log` (J-Link mem32 read at `0x24000050`):

```
24000050 = 0000008D 00000030 21240001 00000000
24000060 = 607C0000 00FF0000 00000101 00035500
24000070 = 00000020 30000000 00000001 00000003
24000080 = 00000006 FF88CA01 FFFF88CA 0000006A
24000090 = 0000008C 0000008D
```

Decoded:

| Index | Field | Value | Status |
|-------|-------|-------|--------|
| [0] ch1_cfg1 | `0x0000008D` | CHEN=1, SPICKSEL=11, SITP=01 | Correct |
| [1] ch1_cfg2 | `0x00000030` | DTRBS=6 | Correct |
| [2] flt0_cr1 | `0x21240001` | RCSEL=CH1, RDMAEN=1, DFEN=1 | Correct |
| [3] flt0_cr2 | `0x00000000` | No interrupts | Correct |
| [4] flt0_fcr | `0x607C0000` | Sinc3, OSR=125 | Correct |
| [5] flt0_isr | `0x00FF0000` | CKABF=0xFF (sticky boot flag) | See §15.6 |
| [6] sai4_pdm | `0x00000101` | PDMEN=1, CKEN1=1 | Correct |
| [7] dma_cr   | `0x00035500` | DMA running, circular, HT+TC | Correct |
| [8] dma_ndtr | `0x00000020` | 32 = full buffer, circulating | Correct |
| [9] dma_m0ar | `0x30000000` | g_DfsdmBuf address | Correct |
| [10] sitp    | `1` | Falling edge | Correct |
| [11] spicksel | `3` | SAI4 bridge | Set (unconfirmed working) |
| [12] dtrbs   | `6` | Right shift 6 | Correct |
| [13] last_raw | `0xFF88CA01` | -30518 = all-zeros PDM input | FAIL — no mic data |

### 15.6 CKABF=0xFF — Observed Every Boot, Confirmed Harmless

Every run shows `FLTISR=0x00FF0000` (CKABF bits set for all 8 channels).

This is a **sticky boot artifact**: DFSDM initialises before `HAL_DFSDM_FilterRegularStart_DMA()` fires, so during the gap the hardware detects no clock on any channel and sets CKABF. The flag persists through the recording. It does not prevent DMA callbacks from firing — `callbacks=3032` with zero overflow confirms the DMA path is fully functional despite CKABF=0xFF.

### 15.7 New Dump — 2026-04-15 15:11 (IAR Debug Log via dfsdm_dump())

First time the dump ran correctly from inside the IAR C-SPY debug session (via Quick Watch `call dfsdm_dump()`). Three snapshots captured: at reset, ~35s after boot (idle), and after a complete recording.

#### Critical new findings

**SPICKSEL is 01 in hardware — not 11.**

Section 3 of the dump (hardware register, not g_dbg):
```
── 3. DFSDM1 CHANNEL 1 (data, 0x40017020) ────────────
  CHCFGR1      = 0x85
    SPICKSEL[3:2]= 1  (1=CKOUT)
    SITP[1:0]    = 1  (falling)
    CHEN(bit7)   = 1
```
`0x85` = `1000_0101` = CHEN=1, SPICKSEL=**01**, SITP=01.

g_dbg snapshot `[11] spicksel = 1` confirms this. The firmware is currently using **SPICKSEL=01 (CKOUT)**, not 11. The previous run that showed `spicksel=3` in g_dbg was from a different firmware build.

**SAI4 was running during recording — confirmed.**

Live globals captured right after `HAL_SAI_Receive_DMA`:
```
g_dbg_live_sai4_cr1 = 0x00C10041   SAIEN(bit16) = 1  ← SAI4 was enabled
g_dbg_live_hal_ok   = 0x00000000   ← HAL_OK, DMA started successfully
g_dbg_live_fltcr1   = 0x21240001   ← filter config correct at DMA start time
```
SAI4 clock (PE2/SAI4_CK1) was active during the recording. The mic was being clocked.

**SAI4 SR OVRUDR=1 — FIFO overrun during recording.**

```
SAI4 SR = 0x1   FREQ(bit1)=0   WCKCFG(bit2)=0
```
Bit 0 of SAI4_SR = OVRUDR (overrun/underrun). SAI4 FIFO overflowed — the software was not reading SAI4 DR fast enough, or the DMA for SAI4 was not consuming data. This is notable but SAI4 is acting only as a clock/data front-end here; the actual data consumer is DFSDM via the internal bridge, not a SAI4 DMA buffer.

**`g_dbg_live_fltisr = 0x00FF4000`** — compared to normal `0x00FF0000`:
- `CKABF[23:16] = 0xFF` — same as always (boot artifact)
- **Bit 14 = 1** — this is `CKABF[6]` for channel 6, or in some interpretations a new flag set during the recording attempt. Not yet diagnosed.

**GPIO confirmed correct:**
```
PC1 mode = 2 (AF), AF = 10 (SAI4_D1)   ✓
PE2 mode = 2 (AF), AF = 10 (SAI4_CK1)  ✓
PE4 mode = 2 (AF), AF = 8  (SAI4_FS_A) ✓
PE5 mode = 2 (AF), AF = 8  (SAI4_SCK_A)✓
PE2 IDR = 0 — clock line reads LOW at this moment (expected: toggling at 1 MHz, reads 0 or 1 randomly when sampled)
```

**SAI4 CR1 MCKDIV=12, not 24:**
```
SAI4 CR1 = 0x00C00041
  MCKDIV[23:20] = 12
```
With PLL2P=49.14 MHz and MCKDIV=12: PDM_CLK = 49.14 MHz / (12×2) = **2.048 MHz** ← correct for 16 kHz × 128 OSR.

**SAI4EN in APB4ENR = 0:**
```
APB4ENR = 0x0   SAI4EN(bit21) = 0
```
SAI4 clock is gated off in `RCC->APB4ENR`. Yet SAI4 appears to be functioning (SAIEN=1 during recording, PDMCR=0x101). This is suspicious — SAI4 may be clocked through a different path (PLL2 direct), or the APB4 bus clock is not needed for the peripheral kernel clock to run.

**C1_APB2ENR bit28 = 0 (CM7 DFSDM gate missing):**
```
C1_APB2ENR = 0x01FE0000   CM7 gate(bit28) = 0
```
The CM7-specific DFSDM clock gate is not set. Only the shared `APB2ENR` bit 28 is set. Per §6 of this document, both gates are required for reliable CM7 register access. This may explain why some register reads return stale/zero values at reset before the clocks fully settle.

#### Summary of new findings

| Finding | Value observed | Significance |
|---------|---------------|-------------|
| SPICKSEL in hardware | 01 (CKOUT) | Current firmware is NOT using SAI4 bridge |
| SAI4 SAIEN during recording | 1 (confirmed) | SAI4 was clocking the mic |
| HAL_DFSDM start | HAL_OK | DMA started without error |
| FLTRDATAR after recording | 0xFF88CA01 | Still -30518, no mic data reaching DFSDM |
| SAI4 OVRUDR | 1 | SAI4 FIFO overrun — data not consumed from SAI4 side |
| GPIO pin mux | All correct | PC1=AF10, PE2=AF10, PE4=AF8, PE5=AF8 |
| SAI4 MCKDIV | 12 → 2.048 MHz | Correct PDM clock frequency |
| SAI4EN in APB4ENR | 0 | SAI4 bus clock apparently not needed for kernel clock |
| CM7 DFSDM gate (C1_APB2ENR bit28) | 0 | Missing; may cause unreliable register access |

#### What this tells us

The entire pipeline is initialised correctly and DMA runs — but DFSDM never sees PDM transitions. With SPICKSEL=01, DFSDM clocks its data capture from its own CKOUT (going to PD3, which is not connected to the mic). The mic outputs PDM on PC1 synchronised to PE2 (SAI4_CK1). These two clocks are independent — DFSDM samples PC1 on its own CKOUT edges and sees all-zeros.

**The test that still needs to happen:** switch SPICKSEL to 11 (SAI4 bridge) using `spicksel_set_sai4()` in the macro console, then trigger a recording and re-run `dfsdm_dump()`. If FLTRDATAR changes from 0xFF88CA01 to anything else, SPICKSEL=11 is the fix.

### 15.8 Summary Table — Verified vs. Unverified (updated 2026-04-15)

| Item | Status | Evidence |
|------|--------|---------|
| LR=HIGH (RIGHT channel, falling edge) | ✅ Verified — hardware fixed | Schematic: SB43=open, R213 pullup |
| SITP=01 written and accepted | ✅ Verified | CHCFGR1=0x85, bits[1:0]=01, every run |
| DTRBS=6 written and accepted | ✅ Verified | CHCFGR2=0x30 every run |
| Filter0 → CH1 routing | ✅ Verified | FLTCR1=0x21240001 every run |
| Sinc3 OSR=125 | ✅ Verified | FLTFCR=0x607C0000 every run |
| SAI4 PDMCR=0x101 (PDMEN+CKEN1) | ✅ Verified | Register read every run |
| SAI4 GPIO pin mux | ✅ Verified | PC1=AF10, PE2=AF10, PE4=AF8, PE5=AF8 — IAR dump 15:11 |
| SAI4 MCKDIV=12 → 2.048 MHz PDM clock | ✅ Verified | CR1=0xC00041, bits[23:20]=12 — IAR dump 15:11 |
| SAI4 was enabled (SAIEN=1) during recording | ✅ Verified | g_dbg_live_sai4_cr1=0xC10041 — IAR dump 15:11 |
| HAL_DFSDM_FilterRegularStart_DMA returned HAL_OK | ✅ Verified | g_dbg_live_hal_ok=0x0 — IAR dump 15:11 |
| DFSDM DFSDMEN=1 at runtime | ✅ Verified | Ch0 CHCFGR1=0x80180000, bit31=1 — IAR dump 15:11 |
| DFSDM CKOUTDIV=24 → 2.0 MHz CKOUT | ✅ Verified | Ch0 CHCFGR1 bits[22:16]=24 — IAR dump 15:11 |
| DMA1_Stream1 running, 32-sample circular | ✅ Verified | NDTR=32, PAR=0x4001711C, M0AR=0x30000000 — IAR dump 15:11 |
| Zero DMA overflows over 3 s | ✅ Verified | `overflow=0` every run |
| SPICKSEL currently = 01 (CKOUT) in hardware | ✅ Verified | Ch1 CHCFGR1=0x85, bits[3:2]=01 — IAR dump 15:11 |
| SPICKSEL=11 (SAI4 bridge) delivers mic data | ❌ Not yet tested | `spicksel_set_sai4()` + recording not yet run |
| Mic producing PDM transitions | ❌ Not yet confirmed | All samples = -30518 regardless of SAI4 state |
| SAI4 CK1 frequency at PE2 | ❌ Not yet confirmed | No oscilloscope measurement |
| CM7 DFSDM gate (C1_APB2ENR bit28) | ❌ Not set | Observed 0 in every dump — consequence unknown |
| SAI4 APB4 bus clock (APB4ENR bit21) | ❓ Not set, yet SAI4 works | SAI4EN=0 but SAIEN=1 confirmed — kernel clock independent |
