# Clock-to-Mic Diagnostics — BEFORE RECORDING (BP1)

## Overview
**When to use:** After firmware load, **before pressing blue button**  
**What it checks:** Initialization state, GPIO config, RCC clocks, power domains  
**Duration:** ~30 seconds, non-intrusive

---

## BP1 Checklist (main.c:226 entry)

### 1. Clock Source Configuration

#### RCC_CR (System Clock Status)
```
Address: 0x58024400
Expected: PLL1 and HSI64 ready (bits 24, 25 should be 1)
  HSIRDY[1]   = 1  ✓ HSI64 ready
  PLL1RDY[24] = 1  ✓ PLL1 ready (may not be used, but good to have)
```
**If HSIRDY = 0:**
- ✗ HSI64 not running → system clock source is broken
- **Fix:** Check hardware oscillator, RCC initialization

#### RCC_CFGR (System Clock Divider)
```
Address: 0x58024410
Expected: Main clock selected and running smoothly
  SWS[5:3] = 3 (PLL1)  or  1 (HSI64)  ✓
  HPRE[3:0] = 0 (no division)  ✓
```

#### RCC_D3CCIPR (D3 Domain Peripheral Clock Selection) — **CRITICAL**
```
Address: 0x58024854
Expected: SAI4A kernel clock = HSI64
  SAI4ASEL[7:5] = 4  (100 binary = 4 decimal)  ✓
  
Values: 0=pclk, 1=HSI, 2=HSE, 3=PLL2, 4=PLL3
  Want: 4 (HSI64, labeled as PLL3 in some docs but is actually HSI64)
```
**If SAI4ASEL ≠ 4:**
- ✗ SAI4 is using wrong clock source (may drift or be too slow)
- **Fix:** Ensure CubeMX RCC config has D3CCIPR SAI4ASEL = HSI64

### 2. Power Domain Status

#### PWR_D3CR (Power Domain 3 Control)
```
Address: 0x58024818
Expected: D3 domain powered and ready
  VOSRDY[13] = 1  ✓ Voltage regulator ready
  VOS[1:0]   = 1  ✓ Low-power mode active (ok for always-on domain)
```
**If VOSRDY = 0:**
- ✗ D3 voltage regulator fault
- **Fix:** Check hardware power supply, reset board

#### PWR_VOSR (Voltage Scaling Output)
```
Address: 0x58024C1C
Expected: Core voltage stable
  VOSRDY[13] = 1  ✓
```

### 3. GPIO PE2 Configuration (Physical Clock Output)

#### GPIOE_MODER (Mode: must be Alternate Function)
```
Address: 0x58020000
Expected: PE2 = Alternate Function mode
  MODER[5:4] = 0b10  (2)  ✓ Alternate Function
  
  Values: 0=input, 1=output, 2=AF, 3=analog
```
**If MODER[5:4] ≠ 2:**
- ✗ PE2 not in AF mode → GPIO will input or analyze, not output SAI4 clock
- **Fix:** Ensure CubeMX GPIO config: PE2 → AF10

#### GPIOE_AFR[0] (Alternate Function Selection for PE2)
```
Address: 0x58020020
Expected: PE2 routed to SAI4_CK1
  AFR[11:8] = 0xA  (10 decimal = AF10)  ✓
  
  AF10 = SAI4_CK1 (PDM clock output)
```
**If AFR[11:8] ≠ 0xA:**
- ✗ PE2 routed to wrong function (I2C, UART, other SAI, etc.)
- **Fix:** Ensure CubeMX GPIO config: PE2 Alternate Function = AF10 (SAI4_CK1)

#### GPIOE_OSPEEDR (Slew Rate: High Speed for 2 MHz)
```
Address: 0x58020008
Expected: High speed to maintain 2 MHz edge integrity
  OSPEEDR[5:4] = 0b11  (3)  ✓ Very High Speed
  
  Values: 0=low, 1=medium, 2=high, 3=very high
```
**If OSPEEDR[5:4] < 3:**
- ⚠ May work but risky—edge timing degraded at 2 MHz
- **Fix:** Set to High Speed (3) in CubeMX GPIO

#### GPIOE_PUPDR (Pull-Up/Pull-Down)
```
Address: 0x58020009
Expected: No pull (clock is active driven by SAI4)
  PUPDR[5:4] = 0b00  (0)  ✓ No pull
```

### 4. SAI4 Configuration (Master Clock Generator)

#### SAI4_CR1 (Control Register 1) — **CRITICAL**
```
Address: 0x5800D804
Expected (at boot, before SAIEN): Configuration set, not yet enabled
  MODE[1:0]      = 1  (01)  ✓ RX Master (generates clock)
    Values: 0=TX Master, 1=RX Master, 2=TX Slave, 3=RX Slave
  
  MCKDIV[8:4]    = 16  (0x10)  ✓ Master clock divider
    Calculation: MCLK_in / MCKDIV = MCLK_out
                 64 MHz / 16 = 4 MHz (intermediate)
                 Then SAI divides again by 2 → 2 MHz PDM clock ✓
  
  SAIEN[16]      = 0   ✓ Not enabled yet (will be 1 during recording)
```
**If MODE ≠ 1:**
- ✗ SAI4 not in Master Receiver mode (won't generate clock)
- **Fix:** CubeMX: SAI4 Block A → Mode = Master Receiver

**If MCKDIV ≠ 16:**
- ✗ Clock output frequency wrong (e.g., 4 MHz instead of 2 MHz)
- **Fix:** Calculate: desired_freq = 64 MHz / (MCKDIV × 2)
         For 2 MHz: MCKDIV = 16

#### SAI4_FRCR (Frame Configuration)
```
Address: 0x5800D808
Expected: Frame format for PDM (1-bit per clock)
  FSOFF[4]  = 0  ✓ FS starts at first bit
  FSP[5]    = 0  ✓ FS active low (normal)
  FSALL[9:0] = 0 ✓ FS duration 1 bit
```

#### SAI4_SLOTR (Slot Configuration)
```
Address: 0x5800D80C
Expected: 1-slot mono (PDM has 1 bit per clock)
  SLOTEN[15:0] = 0x0001  ✓ Slot 0 only
  SLOTSZ[5:4]  = 0      ✓ 8-bit slot width
```

### 5. RCC Clock Gating (is SAI4 powered?)

#### RCC_D3AMR (D3 Autonomous Mode — Peripherals that stay on in sleep)
```
Address: 0x58024838
Expected: SAI4 not clock-gated by system
  SAI4EN[0] = 1  ✓ SAI4 stays powered in D3 domain
```
**If SAI4EN = 0:**
- ✗ SAI4 will be powered down during recording (PE2 goes flat)
- **Fix:** CubeMX RCC: D3 peripheral check → SAI4 enabled in D3AMR

### 6. BDMA Memory Buffer (Pre-Recording)

#### BDMA_CM0AR (Memory Buffer Address)
```
Address: 0x58025010
Expected: D3 domain buffer (always powered)
  Upper byte[31:24] = 0x38  ✓ D3 domain
  
  Values: 0x38 = D3 (permanent), 0x30 = D2 (may sleep)
```
**If [31:24] ≠ 0x38:**
- ⚠ Buffer in D2 or lower → will lose clock during system sleep
- **Fix:** Allocate s_sai4KickBuf in D3 SRAM (0x38000000+)

### 7. DFSDM1 Pre-Config (Filter not yet running)

#### DFSDM_CHCFGR1 (Channel Configuration 1)
```
Address: 0x40017000
Expected: Clock source and edge configured
  SPICKSEL[2:0] = 1  ✓ External SAI4 clock (PE2)
    Values: 0=internal, 1=external (CKOUT/PE2)
  
  SITP[5:4]      = 1  ✓ Falling edge sampling
    Values: 0=rising, 1=falling, 2=both
  
  CHEN[7]        = 1  ✓ Channel enabled
```
**If SPICKSEL ≠ 1:**
- ✗ DFSDM not listening to PE2 clock
- **Fix:** CubeMX: DFSDM Channel 1 → SPICKSEL = External

**If SITP ≠ 1:**
- ✗ DFSDM sampling on wrong edge (will get garbage data)
- **Fix:** Match microphone PDM spec (falling edge = normal)

#### DFSDM_FLTCR1 (Filter Control 1)
```
Address: 0x40017100
Expected: Filter configured, not yet enabled
  DFEN[0]        = 0  ✓ Not enabled (will be 1 during record)
  RDMAEN[1]      = 0  ✓ DMA not armed (will be 1 during record)
  RSWSTART[2]    = 0  ✓ Not started (will be 1 during record)
  RCONT[18]      = 1  ✓ Continuous mode configured
  FAST[29]       = 1  ✓ Fast mode configured
```

---

## Pre-Recording Verification Flow

```
START (after firmware load)
  │
  ├─→ Check RCC_D3CCIPR[7:5] = 4 (HSI64)
  │    └─→ NO  → ✗ Clock source wrong → FIX RCC config
  │    └─→ YES → ✓
  │
  ├─→ Check GPIOE_MODER[5:4] = 2 (AF mode)
  │    └─→ NO  → ✗ PE2 not in AF mode → FIX GPIO config
  │    └─→ YES → ✓
  │
  ├─→ Check GPIOE_AFR[11:8] = 0xA (AF10=SAI4)
  │    └─→ NO  → ✗ PE2 routed wrong → FIX GPIO config
  │    └─→ YES → ✓
  │
  ├─→ Check SAI4_CR1[1:0] = 1 (RX Master)
  │    └─→ NO  → ✗ SAI4 not master → FIX SAI4 config
  │    └─→ YES → ✓
  │
  ├─→ Check SAI4_CR1[8:4] = 16 (MCKDIV)
  │    └─→ NO  → ✗ Clock divider wrong → FIX SAI4 config
  │    └─→ YES → ✓
  │
  ├─→ Check RCC_D3AMR[0] = 1 (SAI4 not gated)
  │    └─→ NO  → ✗ SAI4 will be powered down → FIX RCC D3AMR
  │    └─→ YES → ✓
  │
  ├─→ Check BDMA_CM0AR[31:24] = 0x38 (D3 domain)
  │    └─→ NO  → ⚠ Buffer may sleep → Use D3 SRAM
  │    └─→ YES → ✓
  │
  ├─→ Check DFSDM_CHCFGR1 SPICKSEL = 1 (PE2)
  │    └─→ NO  → ✗ DFSDM not on PE2 → FIX DFSDM config
  │    └─→ YES → ✓
  │
  ├─→ Check DFSDM_CHCFGR1 SITP = 1 (falling edge)
  │    └─→ NO  → ⚠ Wrong edge → data corrupted → FIX DFSDM config
  │    └─→ YES → ✓
  │
  └─→ ✓ ALL CHECKS PASSED → Ready to record!
      Press blue button (PC13)
```

---

## Troubleshooting: Init Phase Failures

### "RCC_D3CCIPR shows wrong SAI4ASEL"

**Cause:** CubeMX project mismatch or RCC not initialized  
**Check:**
```
1. CubeMX → Clock Configuration → Peripheral Clock Select
2. Find SAI4 in the tree
3. Set to HSI64 (not PLL, not PCLK)
4. Generate code
5. Rebuild
```

### "GPIOE_MODER[5:4] = 0 (input mode)"

**Cause:** GPIO initialized as input instead of AF  
**Check:**
1. CubeMX → Pinout & Configuration → PE2
2. Set Mode = Alternate Function
3. Set Alternate Function = AF10 (SAI4_CK1)
4. Set Output Speed = Very High
5. Generate code

### "SAI4_CR1[8:4] ≠ 16 (wrong MCKDIV)"

**Cause:** SAI4 block clock divider misconfigured  
**Check:**
```
Clock math:
  PER_CK (from RCC_D3CCIPR) = 64 MHz (HSI64)
  SAI4_MCKDIV = 16
  MCLK = 64 MHz / 16 = 4 MHz
  SAI4 internal divide by 2 → PDM_CLK = 2 MHz ✓
```
**If MCKDIV ≠ 16:**
- CubeMX → SAI4_BlockA → Clock → MCKDIV = 16

### "RCC_D3AMR[0] = 0 (SAI4 clock-gated)"

**Cause:** SAI4 not marked as always-on  
**Check:**
1. CubeMX → RCC → D3 Autonomous Mode
2. Ensure SAI4 is checked
3. Generate code

### "BDMA_CM0AR[31:24] ≠ 0x38"

**Cause:** BDMA memory buffer allocated in wrong domain  
**Check:**
```c
// voice_recorder.c
// Current: probably at 0x30004100 (D2 SRAM1)
// Should be: 0x38000000+ (D3 permanent)

// Define in linker script:
__no_init uint8_t s_sai4KickBuf[8] @ 0x38003800;
```

---

## How to Use BP1

1. **Load firmware in IAR**
2. **Run to main() — do NOT press blue button yet**
3. **Let it hit BP1** (automatically via macro)
4. **Review macro output:**
   - Are all addresses shown? ✓
   - Any red flags? (values ≠ expected)
5. **Continue execution** (don't trigger recording yet)
6. **Check output against this guide**
7. **If any mismatches → fix CubeMX config → rebuild**

---

## Success Criteria (BP1 Output)

All of these must match before pressing blue button:

```
✓ RCC_D3CCIPR[7:5] = 4
✓ RCC_D3AMR[0] = 1
✓ GPIOE_MODER[5:4] = 2
✓ GPIOE_AFR[11:8] = 0xA
✓ GPIOE_OSPEEDR[5:4] = 3
✓ SAI4_CR1[1:0] = 1
✓ SAI4_CR1[8:4] = 16
✓ BDMA_CM0AR[31:24] = 0x38
✓ DFSDM_CHCFGR1 SPICKSEL = 1
✓ DFSDM_CHCFGR1 SITP = 1
```

If all checks pass → **ready for recording phase** (see `clock_to_mic_AFTER_REC.md`)

---

**Next Step:** After this passes, run `clock_to_mic_AFTER_REC.md` for BP2 and active-recording diagnostics.
