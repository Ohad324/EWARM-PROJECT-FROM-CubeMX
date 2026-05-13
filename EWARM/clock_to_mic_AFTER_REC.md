# Clock-to-Mic Diagnostics — AFTER PRESSING BUTTON (BP2 + Recording)

## Overview
**When to use:** After pressing blue button (PC13) or triggering recording  
**What it checks:** BDMA activation, DMA initialization, SAI4 clock output, DFSDM data flow  
**Duration:** ~3 seconds (recording), then post-check

---

## BP2 Breakpoint Sequence (voice_recorder.c:771 entry)

### Stage 1: Pre-Activation State (Before HAL Calls)

At this point, VoiceRecTask has just been notified and is about to call `Start_Recording_Pipeline()`.
- BDMA is NOT active yet (EN = 0)
- DMA1_Stream1 is NOT active yet (EN = 0)
- SAI4 is NOT enabled yet (SAIEN = 0)
- PE2 is flat (no clock)

#### GPIO PE2 Sanity Check
```
GPIOE_MODER[5:4]   (should match BP1 value)
GPIOE_AFR[11:8]    (should be 0xA)
GPIOE_OSPEEDR[5:4] (should be 3)
```
**If changed from BP1:**
- ✗ GPIO configuration corrupted between boot and recording
- **Fix:** Check if any code modifies GPIO PE2

#### RCC Clock Gate (Still No Clock, But Path Should Be Open)
```
RCC_D3CCIPR[7:5]  = 4   ✓ HSI64 selected
RCC_D3AMR[0]      = 1   ✓ SAI4 not clock-gated
RCC_CR            = 0x... ✓ HSI64 still running
```

#### SAI4_CR1 Pre-Activation State
```
Address: 0x5800D804
SAI4_CR1 = 0x...
  MODE[1:0]    = 1  ✓ RX Master
  MCKDIV[8:4]  = 16 ✓ Clock divider set
  SAIEN[16]    = 0  ✓ Not enabled yet (IMPORTANT!)
  
  After next instruction (HAL_SAI_Receive_DMA), SAIEN will become 1
```

#### DFSDM1 Pre-Armed State
```
DFSDM_FLTCR1 = 0x...
  DFEN[0]      = 1  ✓ Filter enabled
  RDMAEN[1]    = 1  ✓ DMA mode selected
  RSWSTART[2]  = 0  ✓ Not started yet
  RCONT[18]    = 1  ✓ Continuous mode
  FAST[29]     = 1  ✓ Fast mode

DFSDM_ISR = 0x...
  CKABF[19]    = 0  ✓ No clock missing yet (DFSDM doesn't see clock yet, but no error)
  ROVRF[3]     = 0  ✓ No overrun
```

#### BDMA1 Pre-Armed State
```
BDMA1_CCR = 0x...
  EN[0]        = 0  ✓ Not armed yet

BDMA1_CNDTR      = 8  ✓ Will transfer 8 bytes (s_sai4KickBuf)
BDMA1_CPAR       = 0x5800DA28  ✓ SAI4_RDR (read buffer)
BDMA1_CM0AR      = 0x38xxxxxx  ✓ D3 domain memory buffer

BDMA_ISR = 0x...
  TCIF[4]      = 0  ✓ Transfer complete flag (will pulse when active)
```

#### DMA1_Stream1 Pre-Armed State
```
DMA1_S1_CR = 0x...
  EN[0]        = 0  ✓ Not started yet
  CIRC[8]      = 1  ✓ Circular mode enabled
  TCIE[4]      = 1  ✓ TC interrupt enabled
  HTIE[3]      = 1  ✓ HT interrupt enabled

DMA1_S1_NDTR     = 128  ✓ 128 samples per half-buffer
DMA1_S1_PAR      = 0x40017118  ✓ DFSDM FLTRDATAR
DMA1_S1_M0AR     = 0x30004000  ✓ s_DfsdmBuf destination
DMA1_LISR        = 0x...  (status, may show old flags)
```

---

### Stage 2: HAL Calls Executed (Next 2 Lines)

```c
Line N:   HAL_SAI_Receive_DMA(&hsai_BlockA4, (uint8_t *)s_sai4KickBuf, 8u);
          ↓
          BDMA1_CCR.EN → 0 to 1
          SAI4_CR1.SAIEN → 0 to 1
          PE2 → flat to 2.0 MHz clock starts

Line N+1: HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0, ...);
          ↓
          DFSDM_FLTCR1.RSWSTART → 0 to 1
          DFSDM_FLTCR1.DFEN → stays 1
          DMA1_S1_CR.EN → 0 to 1
          DFSDM starts seeing PE2 clock
```

**CRITICAL:** Both must complete successfully within 1 µs.

---

## Active Recording State Checks (During 3-Second Recording)

### Register Watch Points (Poll Every 100 ms)

#### SAI4 Status (Address: 0x5800D804 + 0x04 = 0x5800D808 → SR)

```
SAI4_SR[4]  OVRUDR (Overrun Flag)
  = 0  ✓ FIFO draining OK
  = 1  ✗ SAI4 FIFO filled, PE2 clock STOPPED
       Reason: BDMA not draining fast enough
       Fix: 
         - Check BDMA priority vs other DMAs
         - Lower SAI4 clock (if OSR can tolerate)
         - Check BDMA address in D3 domain
```

#### SAI4 Status (Continuation)

```
SAI4_SR[6]  AFDET (Anticipated Frame Detection)
  = 0  ✓ Normal
  = 1  ⚠ Frame sync mismatched
       Reason: SAI4_FRCR or SLOTR config wrong
       Fix: Verify SAI4 frame configuration
```

#### BDMA Status (Address: 0x58025400 = BDMA_ISR)

```
BDMA_ISR[4] TCIF (Transfer Complete Interrupt Flag)
  Should toggle every ~63 µs (at 2 MHz clock × 8 bytes = 32 µs, plus ISR overhead)
  
  If TCIF never sets:
    ✗ BDMA not firing (no kicks to SAI4)
    Reason: 
      - BDMA_CCR.EN didn't go to 1
      - BDMA buffer address in D3 but D3 domain is sleep-gated
      - BDMA clock disabled
```

#### DFSDM Status (Address: 0x40017118 = DFSDM_ISR)

```
DFSDM_ISR[19] CKABF (Clock Absent Flag)
  = 0  ✓ DFSDM sees the 2.0 MHz clock
  = 1  ✗ DFSDM: "No clock detected!"
       Reason:
         - SAI4_CR1[16] SAIEN lost (PE2 died)
         - PE2 freq not 2 MHz (scope it!)
         - DFSDM_CHCFGR1 SPICKSEL ≠ 1 (not listening to PE2)
         - DFSDM_CHCFGR1 SITP ≠ 1 (wrong edge)
       Fix:
         - Oscilloscope PE2 → must be 2.0 MHz
         - Check SAI4_SR[4] OVRUDR (is BDMA running?)
         - Check BDMA_ISR[4] TCIF (pulses every 32 µs?)
```

#### DFSDM Status (Continuation)

```
DFSDM_FLTCR1[2] RSWSTART (Regular SW Start)
  = 1  ✓ Filter conversion running
  = 0  ✗ Conversion not started (HAL_DFSDM_FilterRegularStart_DMA failed)
```

#### DMA1_Stream1 Status (Address: 0x40020028 = DMA1_S1_CR)

```
DMA1_S1_CR[0] EN (Enable)
  = 1  ✓ DMA running
  = 0  ✗ DMA not started or stopped mid-transfer
```

#### DMA1_Stream1 NDTR (Data Counter)

```
DMA1_S1_NDTR: **Watch this value over 1 second**
  
  Expected behavior:
    T=0:     NDTR = 128 (fresh from HAL)
    T=100ms: NDTR = 100 to 50 (actively decrementing)
    T=500ms: NDTR = ~60 (halfway through a half-buffer)
    T=1000ms: NDTR = ~90 (on second half-buffer)
  
  If NDTR = 128 (unchanged for >10ms):
    ✗ DMA stalled (not getting data from DFSDM)
    Reason:
      - DFSDM not producing samples (check CKABF)
      - DFSDM_FLTCR1[2] RSWSTART = 0
      - DMA configuration error
  
  If NDTR = 0 (and stays 0):
    ? Circular reload issue or end-of-transfer
    Check: DMA1_S1_CR[8] CIRC = 1 (should be circular)
```

#### PE2 Oscilloscope Check

```
Signal: GPIO PE2 (physical pin, not register)
  
Expected:
  - Continuous 2.0 MHz ± 2% (1.96 to 2.04 MHz)
  - 50% duty cycle
  - Clean edges (not sloped)
  - Present for full 3+ seconds
  
If no signal:
  Step 1: Check SAI4_CR1[16] SAIEN = 1
          If 0 → SAI4 was disabled (BDMA didn't work)
  Step 2: Check BDMA_ISR[4] TCIF pulsing
          If not → BDMA stalled
  Step 3: Check SAI4_SR[4] OVRUDR = 0
          If 1 → FIFO filled, PE2 clock killed
  Step 4: Check RCC_D3AMR[0] = 1
          If 0 → SAI4 clock-gated by system sleep

If 4 MHz instead of 2 MHz:
  → MCKDIV = 8 instead of 16
  → Mic will oversample (OSR = 62.5 instead of 125)
  → Audio will be distorted or noisy

If 1 MHz instead of 2 MHz:
  → MCKDIV = 32 instead of 16
  → Mic will undersample (OSR = 250 instead of 125)
  → Audio will be filtered/muffled
```

---

## Quick Diagnosis Tree (During Recording)

```
START: Blue button pressed

  ├─→ Oscilloscope PE2: Is there a clock?
  │    │
  │    ├─ NO (flat) → Go to [A] below
  │    │
  │    └─ YES → Check frequency
  │         │
  │         ├─ 2.0 MHz ±2%  → ✓ Continue to [B]
  │         ├─ 4 MHz       → Wrong MCKDIV (=8, should be 16)
  │         └─ 1 MHz       → Wrong MCKDIV (=32, should be 16)
  │
  └─→ [B] Check g_DfsdmBuf @ 0x30004000
       │
       ├─ Contains non-zero values that change
       │    └─ ✓ RECORDING SUCCESSFUL! Go to [C]
       │
       └─ All zeros or not changing
            └─ DFSDM producing no data. Check [D] below
```

### [A] PE2 Clock Absent (Flat Signal)

```
Step 1: Check BDMA_ISR[4] TCIF
  = 0 (no pulse) → BDMA not running → go to A1
  = 1 (pulsing)  → BDMA running, but clock stopped → go to A2

A1: BDMA Not Running
    Check: SAI4_CR1[16] SAIEN
      = 0 → SAI4 didn't enable (HAL_SAI_Receive_DMA failed)
           Check: g_dbg_live_hal_ok
      = 1 → SAI4 on but BDMA not moving data
           Check: BDMA_CCR[0] EN
      
A2: BDMA Running But Clock Lost
    Check: SAI4_SR[4] OVRUDR
      = 1 → FIFO overflowed, PE2 clock stopped (BDMA too slow)
           Fix: Check BDMA priority, D3 domain status
      = 0 → BDMA draining OK, but PE2 output disabled anyway
           Check: GPIO PE2 configuration (did it revert?)
           Check: RCC_D3AMR[0] (domain sleep?)
```

### [B] PE2 Has Clock (2.0 MHz), Continue to Data Check

```
Check: g_DfsdmBuf at 0x30004000
  Read first 8 values (32-bit each):
    g_DfsdmBuf[0] to g_DfsdmBuf[7]
  
  If ANY are non-zero and CHANGING every 4 ms:
    ✓ Recording working! Go to [C]
  
  If ALL zero or NOT CHANGING:
    ✗ DFSDM producing no data → go to [D]
```

### [C] Recording Successful (Both Clock and Data)

```
Monitor during 3-second recording:
  ✓ PE2 stays 2.0 MHz
  ✓ g_DfsdmBuf keeps updating
  ✓ SAI4_SR[4] OVRUDR stays 0
  ✓ DFSDM_ISR[19] CKABF stays 0
  ✓ DMA1_S1_NDTR keeps decrementing and reloading
  ✓ Elapsed time shows ~3 seconds
  
After recording stops:
  ✓ Check g_AudioBuf @ 0x30020000
    Should contain 48,000 int16_t values
    First few should be small (<500 max)
  
  ✓ Check SD card for REC_XXX.wav
    File size ~96 KB (48000 samples × 2 bytes)
```

### [D] No Data in g_DfsdmBuf (Clock Present But No PCM)

```
Step 1: Check DFSDM_ISR[19] CKABF
  = 1 → DFSDM doesn't see clock (but scope shows 2 MHz?)
       Likely: Wrong edge (SITP) or wrong source (SPICKSEL)
       Fix: Verify DFSDM_CHCFGR1:
            SPICKSEL[2:0] = 1 (PE2)
            SITP[5:4] = 1 (falling edge)
       Or: Oscilloscope might show clock at SAI4 output, but
           not reaching DFSDM input (check PC1 DATIN1)
  
  = 0 → DFSDM sees clock
       Check Step 2 below

Step 2: Check DFSDM_FLTCR1[2] RSWSTART
  = 0 → Filter didn't start
       Check: g_dbg_live_hal_ok (was HAL_DFSDM_FilterRegularStart_DMA OK?)
       Fix: Rebuild, re-flash
  = 1 → Filter running
       Check Step 3

Step 3: Check DMA1_S1_CR[0] EN
  = 0 → DMA didn't start
       Check: HAL_DFSDM_FilterRegularStart_DMA return code
  = 1 → DMA running
       Check DMA1_S1_NDTR decrementing?
       Check: Read DFSDM_FLTRDATAR directly @ 0x40017118+0x18
              Should see changing values (32-bit PCM samples)
              If values stuck or zero → DFSDM decimator stalled
```

---

## Post-Recording Autopsy

After 3 seconds or when recording stops:

### Check File Written to SD Card

```bash
# On host PC, via SD card USB reader or FAT32 mount
ls -la REC_001.wav
# Should show: -rw-r--r-- ... 98304 bytes (48000×2 + 512 header)

# Verify with Windows Media Player or Audacity
# Should play as recognizable speech/sound (not silence)
```

### Check g_AudioBuf @ 0x30020000

```c
// Via IAR Memory window or watch expression
int16_t sample = __readMemory16(0x30020000 + (i*2), "Memory");

// Expected:
//   Sample[0..100]:  Values in range -100 to +100 (silence/noise floor)
//   Sample[100..500]: Values in range -500 to +500 (speech beginning)
//   Sample[500..end]: Values in range -5000 to +5000 (speech peaks)
//   NOT all zeros
//   NOT all 0x5555 (DMA init pattern)
```

### Check g_AudioHealth Structure

```c
// Check after SDWriteTask completes (g_State = REC_IDLE)
typedef struct {
    uint32_t dfsdm_overruns;      // Should be 0
    uint32_t sd_write_max_ms;     // Should be <20 ms
    uint32_t buffer_misses;       // Should be 0
    uint32_t total_bytes_written; // Should be ~96 KB
} AudioHealth_t;

// These are logged to RTT:
g_AudioHealth.dfsdm_overruns == 0  ✓
g_AudioHealth.sd_write_max_ms < 20 ✓
g_AudioHealth.buffer_misses == 0   ✓
```

### Post-Recording Register State

```
SAI4_CR1[16] SAIEN      = 0  ✓ (stopped after recording)
DFSDM_FLTCR1[2] RSWSTART = 0  ✓ (stopped after recording)
DMA1_S1_CR[0] EN        = 0  ✓ (stopped after recording)
BDMA1_CCR[0] EN         = 0  ✓ (stopped after recording)
```

If any are still 1 after g_State returns to IDLE:
- ⚠ Cleanup incomplete
- Check VoiceRecTask stopping code (line ~440)

---

## Common Failures & Solutions During Recording

| Symptom | Root Cause | Quick Fix |
|---|---|---|
| PE2 stays flat after BP2 | BDMA_CCR.EN = 0 | Check HAL_SAI_Receive_DMA return, BDMA priority |
| PE2 flat after 1-2 sec | SAI4_SR[4] OVRUDR=1 | BDMA too slow (D3 sleep?) or SAI4 clock too fast |
| PE2 shows 4 MHz | SAI4_CR1[8:4]=8 | Verify MCKDIV should be 16 for 2 MHz |
| PE2 present but g_DfsdmBuf zeros | DFSDM_ISR[19] CKABF=1 | Check SPICKSEL=1, SITP=1, or PE2 frequency wrong |
| DMA1_S1_NDTR stuck at 128 | DFSDM not producing | Check RSWSTART=1, DFEN=1, clock present on PC1 (DATIN1) |
| Noisy audio (SAI4_SR[4] toggling) | BDMA starved | Raise BDMA IRQ priority above other DMAs |
| Audio cuts out mid-record | System sleep gated D3 | Check PWR domain, keep D3 always-on during recording |
| File size wrong (not 98 KB) | Recording stopped early | Check VoiceRecTask drain loop (should run ~3 sec) |

---

## Verification Checklist (During Recording)

- [ ] **PE2 Scope:** 2.0 MHz ± 2%, continuous, clean edges
- [ ] **BDMA Status:** BDMA_ISR[4] TCIF pulsing every ~32 µs
- [ ] **SAI4 Status:** SAI4_SR[4] OVRUDR = 0 throughout
- [ ] **DFSDM Status:** DFSDM_ISR[19] CKABF = 0 throughout
- [ ] **DMA Status:** DMA1_S1_CR[0] EN = 1, NDTR decrementing
- [ ] **Data Flow:** g_DfsdmBuf values changing every 4 ms
- [ ] **Post-Recording:** g_State returns to REC_IDLE after 3 sec
- [ ] **Audio File:** REC_XXX.wav exists on SD, ~98 KB size
- [ ] **Audio Quality:** RTT shows dfsdm_overruns=0, no buffer_misses

---

## How to Use BP2

1. **Let macro run to BP1, verify init passed** (see `clock_to_mic_BEFORE_REC.md`)
2. **Continue past BP1** (don't pause yet)
3. **Press blue button (PC13) on board or trigger in simulator**
4. **Macro automatically hits BP2** when Start_Recording_Pipeline() called
5. **Review BP2 output:**
   - All register values shown
   - Pre-activation state captured
6. **Step one line** (over HAL_SAI_Receive_DMA)
   - BDMA_CCR[0] should change 0→1
   - SAI4_CR1[16] should change 0→1
   - PE2 oscilloscope should show 2.0 MHz appear
7. **Step one line** (over HAL_DFSDM_FilterRegularStart_DMA)
   - DMA1_S1_CR[0] should change 0→1
   - DFSDM_FLTCR1[2] RSWSTART should change 0→1
8. **Continue** (remove breakpoint) and let recording complete
9. **When g_State returns to REC_IDLE:**
   - Check g_DfsdmBuf had data
   - Check REC_XXX.wav exists
   - Verify file playable

---

**Previous Step:** Start with `clock_to_mic_BEFORE_REC.md` for init phase checks.
