# Voice Recorder DFSDM Bringup — Testing Chronology

**Session date:** 2026-04-28
**Final outcome:** ✅ Mic working — real PCM data flowing into `s_DfsdmBuf`.

This document captures the diagnostic chain that found and fixed the root cause: a single wrong byte in the GPIO alternate-function register for PC1.

---

## Test 0 — Starting state (commit `aaf0a5e`, "Path C-PC2")

**Configuration:**
- DFSDM1 Channel 0: clock master only — `DFSDMEN=1`, `CKOUTDIV=49` (→ 2.000 MHz on PC2/AF6)
- DFSDM1 Channel 1: data sampler — `CHEN=1`, `SPICKSEL=01` (internal CKOUT), `SITP=01` (falling edge)
- PC1: `GPIO_AF6_DFSDM1` (assumed = DATIN1 per project docs)
- Filter 0: reads channel 1, Sinc³ OSR=125, DMA to `s_DfsdmBuf @ 0x30004000`

**Symptoms:**
- 2.000 MHz visible on PC2 with scope ✓
- PC1 IDR toggles between 0 and 1 across J-Link reads ✓ (mic produces PDM)
- DMA NDTR wraps (0x4F → 0x7F → 0x48) ✓
- PWR_WKUPEPR = 0x00000000 ✓ (no WKUP6 trap)
- ❌ **`FLT0RDATAR` stuck at `0xFF88CA01`**
- ❌ **`s_DfsdmBuf` filled with constant `0xFF88CA01`** repeated

**Decode of `0xFF88CA01`:** bottom byte `0x01` = channel-ID for CH1 (correct, since filter is reading CH1). Top 24 bits `0xFF88CA` = signed −30518. With Sinc³ OSR=125 and DTRBS=6, max-negative saturation =  −125³ / 64 = **−30518 exact**. Filter is seeing constant 0 input.

---

## Test 1 — Discriminator probe ([dfsdm_datapath_probe.jlink](../EWARM/dfsdm_datapath_probe.jlink))

**Goal:** rule out two theories — (A) DMA targets wrong address, (B) CKABF latched and gating data.

**What it does:**
1. Snapshot DMA `CR / NDTR / PAR / M0AR` and DMAMUX C1CR
2. Snapshot DFSDM `FLTCR1 / FLTCR2 / FLTISR / FLTICR` and CH1 `CFGR1 / CFGR2`
3. Snapshot buffer at 0x30004000
4. Write `0x00FF0000` to `FLTICR` (write-1-to-clear all CKABF bits)
5. Sleep 100 ms
6. Re-snapshot to see if data starts flowing after CKABF clear

**Results:**
| Register | Value | Decode |
|----------|-------|--------|
| `DMA1_Stream1->CR`   | `0x0002551F` | EN=1, CIRC=1, MINC=1, P/M=32-bit, prio=high, dir=P→M ✓ |
| `DMA1_Stream1->PAR`  | `0x4001711C` | = `&FLT0RDATAR` ✓ |
| `DMA1_Stream1->M0AR` | `0x30004000` | = `&s_DfsdmBuf[0]` ✓ |
| `DMAMUX1_C1CR`       | `0x00000065` | REQ=101 = DFSDM1_FLT0 ✓ |
| `FLT0CR1`            | `0x21240001` | DFEN, RCH=001=CH1, RDMAEN, RCONT ✓ |
| `FLT0ISR` (post-clear) | `0x00FD4000` | bits[23:16]=0xFD → CKABF[1]=0 (cleared and stayed cleared) ✓ |

**Conclusions:**
- ✅ **Theory A eliminated** — DMA addresses are correct.
- ✅ **CH1's CKABF cleared and stayed cleared** = DFSDM CH1's internal sample clock IS running.
- ❌ But `FLT0RDATAR` STILL stuck at `0xFF88CA01` after CKABF clear → filter genuinely sees constant 0 on DATIN1.

**Implication:** routing inside DFSDM is working, but DATIN1 is not connected to the toggling PC1 signal. **Suspect the GPIO AF mapping.**

---

## Test 2 — GPIO state verification ([gpioc_pc1_check.jlink](../EWARM/gpioc_pc1_check.jlink))

**Goal:** prove PC1 was actually programmed with the AF byte we wrote in code (rule out a different bug overwriting it).

**Result:**
| Register | Value | Decode |
|----------|-------|--------|
| `GPIOC_MODER` | `0xF2AABFEB` | PC1 bits[3:2]=10 (AF mode) ✓, PC2 bits[5:4]=10 (AF mode) ✓ |
| `GPIOC_PUPDR` | `0x00000004` | PC1 bits[3:2]=01 (pull-up) ✓ |
| `GPIOC_AFRL`  | `0x40000660` | PC1 nibble bits[7:4]=**`0x6`** (AF6 written), PC2 bits[11:8]=`0x6` (AF6) |
| PC1 IDR samples (×8) | bit1 = 0,0,0,1,1,0,0,0 | toggles at J-Link sample rate ✓ |

**Conclusion:** GPIO is exactly as written. The bug is in *what AF6 means for PC1*, not in whether AF6 was applied.

---

## Test 3 — User reminder + Gemini's claim

User shared Gemini's mapping claim:
> PC1 AF3 = DFSDM1_CKIN1
> PC1 AF4 = DFSDM1_DATIN0 (the target)
> PC1 AF6 = DFSDM1_DATIN4 (the trap)

This contradicted the project docs (which said PC1/AF6 = DATIN1) and was the first plausible explanation for the −30518 saturation: writing AF6 routes mic data to channel 4 (unused), not channel 1, so DFSDM channel 1 reads a floating/grounded DATIN1 from a different pin.

**Action:** wired in PC1/AF4=DATIN0, reconfigured CH0 as both clock master and data sampler, pointed Filter0 at CH0.

**Result of Test 3 (build + flash + probe):**
| Register | Before | After |
|----------|--------|-------|
| `CH0 CFGR1` | `0x80310080` | **`0x80310085`** ✓ (CHEN, SPICKSEL=01, SITP=01 added) |
| `FLT0CR1`   | `0x21240001` | **`0x20240001`** ✓ (RCH 001→000) |
| `RDATAR` channel-ID byte | `0x01` (CH1) | **`0x00` (CH0)** ✓ — proves filter is now reading CH0 |
| `RDATAR` data | `0xFF88CA01` | **`0xFF88CA00`** — same −30518 saturation, just from CH0 now |

**Conclusion:** Routing change WORKED (channel-ID byte flipped 0x01→0x00 = filter is reading the channel we configured), but the saturation persisted → AF4 is also wrong, or some other issue.

---

## Test 4 — SITP edge swap (rule out timing)

**Hypothesis:** maybe data IS reaching DATIN0 but our SITP=01 (falling) misses the data eye.

**Action:** flipped `SITP` to 00 (rising), rebuilt, flashed.

**Result:**
- `CH0 CFGR1 = 0x80310084` ✓ (SITP=00 applied)
- `RDATAR = 0xFF88CA00` — **identical −30518 saturation**

**Conclusion:** edge timing is NOT the issue. Data is genuinely not reaching DFSDM. Reverted SITP=01.

---

## Test 5 — Independent web verification of AF mapping

**Action:** spawned a research agent to fetch the AF table from authoritative web sources (NOT trust local docs or Gemini).

**Three independent sources converged on the same answer:**
1. Linux kernel `st,stm32-pinctrl` device-tree bindings — STM32_PINMUX entries for PC1
2. ST community thread on H747I-DISCO with MP34DT05 mic — same board, same mic, same problem
3. ST wiki DFSDM device-tree configuration page

**All three say: PC1/AF3 = DFSDM1_DATIN0.**

| Pin/AF | Source A (project docs) | Source B (Gemini) | **Source C (web, 3 confirmations)** |
|--------|-------------------------|-------------------|-------------------------------------|
| PC1/AF3 | unused | CKIN1 | **DATIN0** ✓ |
| PC1/AF4 | unused | DATIN0 ❌ | I2C3_SCL |
| PC1/AF6 | DATIN1 ❌ | DATIN4 | unassigned for DFSDM |

Both prior local sources were wrong. The web converged sources won.

---

## Test 6 — AF3 fix and verification (final)

**Action:** changed PC1 GPIO `Alternate = GPIO_AF3_DFSDM1`, NoPull (per ST examples), rebuilt, flashed, ran probe.

**Result:**
| Register | Value | Status |
|----------|-------|--------|
| `CH0 CFGR1` | `0x80310085` | DFSDMEN | CKOUTDIV=49 | CHEN | SPICKSEL=01 | SITP=01 ✓ |
| `FLT0CR1`   | `0x20240001` | RCH=0=CH0, RDMAEN, RCONT, DFEN ✓ |
| `RDATAR`    | varies sample-to-sample (e.g. `0x00001500`, `0xFFFFF700`, ...) | **NOT stuck** ✓ |
| `s_DfsdmBuf[0..47]` | `0x1100, 0x1300, 0x1500, 0x1500, 0x1600, 0x1500, 0x1200, 0x1000, 0x0F00, ...` then `0x0A00, 0x0A00, 0x0B00, ...` then negative excursions `0xFFFFF900, 0xFFFFF700, ...` | **REAL PCM** — bipolar, low-amplitude (~±5000), slowly varying = silence floor with mic noise ✓ |

**Decode of buffer values:**
- Each entry is 32-bit. Bottom byte = `0x00` = channel-ID for CH0.
- Top 24 bits, signed = the actual PCM sample.
- `0x00001100` = +4352, `0x00001500` = +5376, `0xFFFFF900` = −1792.
- Range ~±6000 in a quiet room, near-zero mean → consistent with a working PDM mic in silence.

**Conclusion:** ✅ Mic is working. End-to-end PDM → DFSDM → DMA → SRAM data path is functional.

---

## Lessons learned

1. **`−30518` saturation is the signature of a constant-input filter.** With Sinc³ OSR=125 and DTRBS=6, max-negative is exactly −30518 = `0xFF88CA00` (with channel-ID byte = 0). Whenever the buffer fills with that exact value, suspect "DFSDM is sampling something disconnected" — the filter literally cannot see any 1s.

2. **The bottom byte of `FLTRDATAR` is the channel-ID** — flipping the AF/filter routing should change that byte. Changes in the channel-ID byte across config tweaks are diagnostic gold.

3. **AF mapping cannot be assumed from CMSIS headers.** `GPIO_AF3_DFSDM1` through `GPIO_AF11_DFSDM1` are all defined as raw constants 3, 4, 6, 11 — they tell you what to *write*, not what *signal* you'll get on a given pin. The pin↔signal mapping is in the chip pinout table in the datasheet, which differs by pin.

4. **Always verify against multiple independent sources before acting on AI-suggested register values.** Both the project's own docs AND Gemini gave wrong AF values for PC1. Three convergent web sources gave the right one. The cost of one extra `WebSearch` is far less than a bad rebuild cycle plus the mental load of debugging the wrong bug.

5. **Use the ST community as a direct lookup for board-specific bringup.** Someone has almost always done this exact bringup before. The H747I-DISCO + MP34DT05 + DFSDM thread answered the question in one paragraph.

---

## Diagnostic scripts (preserved in repo)

- [EWARM/dfsdm_datapath_probe.jlink](../EWARM/dfsdm_datapath_probe.jlink) — Tests 1, 3, 4, 6
- [EWARM/gpioc_pc1_check.jlink](../EWARM/gpioc_pc1_check.jlink) — Test 2
- [EWARM/runs/](../EWARM/runs/) — full chronological logs of every flash+run cycle in this session

## Files that the fix touched

- [CM7/Core/Src/main.c](../CM7/Core/Src/main.c) — `MX_DFSDM1_Init` (CH0 = clock master + data sampler, Filter0 → CH0)
- [CM7/Core/Src/stm32h7xx_hal_msp.c](../CM7/Core/Src/stm32h7xx_hal_msp.c) — PC1 `Alternate = GPIO_AF3_DFSDM1`, `Pull = NOPULL`
