# Clock-to-Mic Debugging Toolkit

## Quick Start

You have **3 files** for complete clock-to-mic + BDMA diagnostics:

### 📊 **1. The C-SPY Macro** — `clock_to_mic_bdma.mac`
**What it does:** Automatically captures register state at 2 critical breakpoints  
**Breakpoint 1:** `main.c:226` — Boot diagnostics  
**Breakpoint 2:** `voice_recorder.c:771` — Pipeline activation  
**How to use:**
```
IAR → Debug → Macros → Execute Macro → clock_to_mic_bdma.mac
Let it run, review console output at each BP
```

### 📖 **2. Init Phase Guide** — `clock_to_mic_BEFORE_REC.md`
**When:** After firmware load, **BEFORE pressing blue button**  
**What it checks:**
- ✓ GPIO PE2 AF configuration
- ✓ RCC clock source (HSI64)
- ✓ SAI4 master mode + MCKDIV
- ✓ BDMA memory in D3 domain
- ✓ DFSDM clock source (PE2) + edge (falling)
- ✓ RCC SAI4 clock-gate status

**Success:** All 10 criteria met → Ready to record

**Example failure:**
```
GPIOE_MODER[5:4] = 0 (not AF mode)
  ✗ PE2 cannot output SAI4 clock
  FIX: CubeMX GPIO config
```

### 🔴 **3. Recording Phase Guide** — `clock_to_mic_AFTER_REC.md`
**When:** After pressing blue button through 3-second recording  
**What it monitors:**
- ✓ PE2 oscilloscope (2.0 MHz output)
- ✓ BDMA activation (CCR.EN → 0 to 1)
- ✓ SAI4 status (OVRUDR = 0, SAIEN = 1)
- ✓ DFSDM status (CKABF = 0, RSWSTART = 1)
- ✓ DMA data flow (NDTR decrementing)
- ✓ PCM samples in g_DfsdmBuf

**Success:** PE2 runs 2 MHz, g_DfsdmBuf fills with non-zero data → File written to SD

**Quick diagnosis tree:**
```
No clock on PE2?
  → Check BDMA_ISR[4] TCIF (pulsing?)
  → Check SAI4_SR[4] OVRUDR (=1 = FIFO overflow, clock killed)
  → Check RCC_D3AMR[0] SAI4EN (=1?)

Clock present but no PCM data?
  → Check DFSDM_ISR[19] CKABF (=1 = clock missing to DFSDM)
  → Check DFSDM_CHCFGR1 SPICKSEL=1 (PE2), SITP=1 (falling edge)
  → Check DMA1_S1_NDTR (decrementing or stuck at 128?)
```

---

## Diagnostic Flowchart

```
┌─ LOAD FIRMWARE ─────────────────┐
│                                  │
├─ Hit BP1 (main.c:226)           │
│  Review: GPIO, RCC, SAI4, BDMA   │
│  Check against BEFORE_REC.md     │
│                                  │
├─ All criteria ✓?                │
│  └─ NO  → Fix CubeMX, rebuild   │
│  └─ YES → Continue              │
│                                  │
├─ Press blue button (PC13)        │
│  or trigger recording            │
│                                  │
├─ Hit BP2 (voice_recorder.c:771)  │
│  Review: pre-activation state    │
│  Step over HAL calls             │
│                                  │
├─ Watch:                          │
│  • PE2 oscilloscope              │
│  • BDMA_CCR[0] → 0 to 1         │
│  • SAI4_CR1[16] → 0 to 1        │
│  • DMA1_S1_CR[0] → 0 to 1       │
│                                  │
├─ Recording runs 3 seconds        │
│  Check against AFTER_REC.md      │
│  Monitor: OVRUDR, CKABF, NDTR   │
│                                  │
├─ Recording completes             │
│  Verify:                         │
│  • g_DfsdmBuf has non-zero data │
│  • REC_XXX.wav on SD card       │
│  • File is ~98 KB               │
│  • Audio is audible             │
│                                  │
└─ ✓ SUCCESS ─────────────────────┘
```

---

## Common Failures

### "No Clock on PE2"

**1. Check Init (BEFORE_REC.md):**
```
GPIOE_MODER[5:4] ≠ 2  → PE2 not in AF mode
GPIOE_AFR[11:8] ≠ 0xA → PE2 not wired to SAI4
RCC_D3AMR[0] = 0       → SAI4 clock-gated
→ FIX: CubeMX GPIO + RCC config
```

**2. Check BP2 Activation:**
```
SAI4_CR1[16] SAIEN stays 0  → HAL_SAI_Receive_DMA failed
BDMA_CCR[0] stays 0         → BDMA not armed
→ Check: HAL return codes, IRQ priorities
```

**3. Check Active State:**
```
PE2 briefly present then stops  → SAI4_SR[4] OVRUDR=1
                                   BDMA not draining FIFO fast enough
                                   → Lower SAI4 clock or raise BDMA priority
```

### "Clock Present but No Data"

**1. Check DFSDM:**
```
DFSDM_ISR[19] CKABF = 1  → DFSDM doesn't see clock
  → Check: SPICKSEL=1 (PE2), SITP=1 (falling edge)
  → Scope: PC1 (DATIN1) should have PDM data
```

**2. Check DMA:**
```
DMA1_S1_NDTR = 128 (unchanged)  → DMA not moving
  → Check: DMA1_S1_CR[0] EN=1
  → Check: DFSDM_FLTCR1[2] RSWSTART=1 (filter running)
  → Check: Read DFSDM_FLTRDATAR directly (stuck?)
```

### "BDMA Kicks Not Firing"

**Symptom:** BDMA_ISR[4] TCIF never pulses

**Cause:**
```
BDMA_CM0AR[31:24] ≠ 0x38  → Buffer not in D3 domain
  → System can sleep, BDMA buffer unreachable
  → PE2 clock dies after 1-2 seconds
  → FIX: Allocate s_sai4KickBuf in 0x38000000+ (D3 permanent)
```

---

## Files at a Glance

| File | Purpose | When to Use |
|------|---------|-------------|
| `clock_to_mic_bdma.mac` | C-SPY macro with 2 BPs | Load firmware → Run → Follow prompts |
| `clock_to_mic_BEFORE_REC.md` | Init phase diagnostics | Before pressing blue button |
| `clock_to_mic_AFTER_REC.md` | Recording phase diagnostics | After button press, during/after 3 sec |
| `clock_to_mic_bdma_README.md` | Register reference + troubleshooting | Reference during any phase |

---

## Register Address Quick Reference

### Clock Path
| Register | Address | Key Bits |
|----------|---------|----------|
| RCC_CR | 0x58024400 | PLL1RDY[24], HSIRDY[1] |
| RCC_CFGR | 0x58024410 | SWS[5:3] |
| **RCC_D3CCIPR** | **0x58024854** | **SAI4ASEL[7:5]=4** |
| RCC_D3AMR | 0x58024838 | SAI4EN[0] |

### GPIO PE2
| Register | Address | Key Bits |
|----------|---------|----------|
| GPIOE_MODER | 0x58020000 | MODER[5:4]=2 (AF) |
| GPIOE_AFR[0] | 0x58020020 | AFR[11:8]=0xA (AF10) |
| GPIOE_OSPEEDR | 0x58020008 | OSPEEDR[5:4]=3 (High) |

### SAI4
| Register | Address | Key Bits |
|----------|---------|----------|
| **SAI4_CR1** | **0x5800D804** | **MODE=1, MCKDIV=16, SAIEN=1** |
| SAI4_SR | 0x5800D80C | OVRUDR[4] (must=0) |

### BDMA1
| Register | Address | Key Bits |
|----------|---------|----------|
| BDMA_CCR | 0x58025004 | EN[0] |
| BDMA_ISR | 0x58025400 | TCIF[4] (pulses every 32µs) |
| BDMA_CM0AR | 0x58025010 | [31:24]=0x38 (D3 domain) |

### DFSDM1
| Register | Address | Key Bits |
|----------|---------|----------|
| **FLTCR1** | **0x40017100** | **DFEN=1, RDMAEN=1, RSWSTART=1, RCONT=1, FAST=1** |
| CHCFGR1 | 0x40017000 | SPICKSEL=1, SITP=1 |
| **ISR** | **0x40017118** | **CKABF[19]=0 (clock present)** |

### DMA1_Stream1
| Register | Address | Key Bits |
|----------|---------|----------|
| DMA_CR | 0x40020028 | EN[0], CIRC[8] |
| **DMA_NDTR** | **0x4002002C** | **Must decrement, not stuck at 128** |
| DMA_LISR | 0x4002000C | Interrupt flags |

---

## RTT/Console Output Indicators

**Watch for these in terminal during recording:**

```
[DFSDM3 PASS]          ✓ VoiceRecTask reached Start_Recording_Pipeline
[SAI4] SAIEN=1         ✓ SAI4 enabled after BDMA start
[BDMA] CCR=...         ✓ BDMA active (EN bit should be 1)
[REC] START trigger=BTN ✓ Recording started
[REC] DFSDM_DONE samples=48000  ✓ Full buffer collected after 3 sec
[SD] SAVING ...        ✓ Writing WAV to SD card
```

If any of these don't appear → Check corresponding macro output

---

## Next Steps

1. **Load macro** → hit BP1 → verify init state
2. **Compare BP1 output** to `clock_to_mic_BEFORE_REC.md` checklist
3. **If init fails** → Fix CubeMX config, rebuild
4. **If init passes** → Continue to BP2
5. **Press blue button** → BP2 fires automatically
6. **Compare BP2 output** to `clock_to_mic_AFTER_REC.md` pre-activation section
7. **Step over HAL calls** → watch registers change
8. **Let recording run** → monitor real-time via `clock_to_mic_AFTER_REC.md` active phase
9. **Verify output** → Check g_DfsdmBuf, RTC_XXX.wav, console logs

---

**Last Updated:** 2026-04-19  
**System:** STM32H747 CM7, SAI4 + DFSDM + BDMA audio pipeline  
**Status:** ✓ Ready for troubleshooting
