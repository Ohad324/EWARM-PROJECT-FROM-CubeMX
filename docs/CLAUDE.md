# CLAUDE.md — STM32H747I-DISCO Voice Recorder

## Project Identity
- Board: STM32H747I-DISCO
- Active core: Cortex-M7
- Toolchain: CubeMX + IAR
- IAR project: `EWARM/STM32H747I-DISCO.ewp`
- Active target: `STM32H747I-DISCO_CM7`

## Documentation Rule — MANDATORY, NO EXCEPTIONS
Before proposing or implementing ANY fix, you MUST consult:
1. **Datasheet DS12931** (Pin Mapping Table)
2. **Reference Manual RM0399** (SAI/DFSDM/DMA Chapters)
3. **Application Note AN5200** (Bus Matrix & Domain Architecture)

---

## Hardware Architecture & Data Pipeline

### 1. Physical Layer & MUX
- **Clock (Output): PE2** → **AF10** (SAI4_CK1) — 2.0 MHz PDM clock to MP34DT05-A mic.
- **Data (Input): PC1** → **AF6** (DFSDM1_DATIN1). *Schematic labels this SAI4_D1/AF10, but AF6 is mandatory to reach the DFSDM filter.*
- **Timing (LR=High):** The schematic pulls LR High. Configure DFSDM to sample on **Falling Edge** (`SITP=01`). [RM0399, P. 2148]

### 2. Current Pipeline — Single-Stage DMA1 (Baseline)

**Goal:** Prove data is moving before adding domain-crossing complexity.

```
MP34DT05-A (PDM mic)
    │  PE2 = 2.0 MHz clock (SAI4_CK1, AF10)
    │  PC1 = PDM data    (DFSDM1_DATIN1, AF6)
    ▼
DFSDM1 Filter0  (Sinc3, OSR=125, SITP=01, SPICKSEL=01)
    │  Decimates PDM → 32-bit PCM @ 16 kHz
    ▼
DMA1_Stream1  (DMAMUX1: DMA_REQUEST_DFSDM1_FLT0, Circular mode)
    │  Peripheral→Memory, 32-bit words, no FIFO
    ▼
s_DfsdmBuf[128]  @ 0x30004000  (D2 SRAM1)
    │  DMA1 is a D2 bus master — it can reach D2 SRAM directly
    ▼
CPU callbacks  (HAL_DFSDM_FilterRegConvHalfCpltCallback / CpltCallback)
    │  StoreDmaChunk → g_AudioBuf → SD card WAV file
```

**Why single-stage first:** If DMA1+MDMA fails, we cannot tell which link broke.
Once non-zero data appears at `0x30004000`, the baseline is proven.

### Clock Gating Rule
- **SAI4 Clock (PE2)** is hardware gated. It will **STALL** if no DMA is draining the DFSDM peripheral. [RM0399, P. 2200]
- If the clock is dead: verify DMA1_Stream1 is running (CCR EN=1) and DFSDM filter is enabled (FLTCR1 DFEN=1).

### GOTCHA — PC1 is also WKUP6 (PWR Standby Wakeup Pin 6)

On STM32H747, **WKUP6 is multiplexed onto PC1** — the same pin we use for DFSDM1_DATIN1 (AF6). If CubeMX has the *"Wake-up Pin 6"* box checked under **Pinout & Configuration → System Core → PWR**, the generated code calls `HAL_PWREx_EnableWakeUpPin(PWR_WAKEUP_PIN6_…)`, which sets **`PWR_WKUPEPR` bit 5 (WKUPEN6)**. Once that bit is set, the PWR block claims PC1 as a deep-sleep wakeup input (with its own pull configuration in `WKUPPUPD6[27:26]`) and overrides the normal AF6 sampling path. DFSDM then sees a stuck DC level and **CKABF** asserts on Channel 1 — classic "filter runs, DMA runs, output stuck at one value" symptom.

**The Fix:** Uncheck that box so the power-management block **releases its deep-sleep claim on PC1** — that disables the wake-up option and leaves PC1 free for DFSDM. In your manual code, ensure the `PWR_WKUPEPR` register has the **WKUP6 bit set to 0**.

```c
CLEAR_BIT(PWR->WKUPEPR, PWR_WKUPEPR_WKUPEN6);   /* detach wakeup detector from PC1 */
```

- Register: `PWR_WKUPEPR` @ `0x58024828` (PWR base `0x58024800` + offset `0x28`).
- Clear bit **5** (`WKUPEN6`). Do not touch bits 0–4 (other WKUPENx) or bits 16–31 (polarity/pull).
- Also verify `WKUPPUPD6` (bits 27:26) reads `00` — a stale pull-up/down on PC1 will equally corrupt the PDM data edge.

**Signature in a register dump:**
- `PWR_WKUPEPR` = any value with bit 5 set → **BAD** (PC1 hijacked)
- `PWR_WKUPEPR` bit 5 = 0 AND bits 27:26 = 00 → **GOOD** (PC1 free for DFSDM).

### 3. Next Stage — MDMA Bridge (Future, after baseline confirmed)
Once `0x30004000` shows non-zero PCM data:
- Add `MDMA_Channel0` triggered by `MDMA_REQUEST_DMA1_Stream1_TC`
- MDMA moves `s_DfsdmBuf` (D2 SRAM1) → `DTCM_PCM_Buffer` (D1 DTCM, `0x20000000`)
- MDMA has no direct DFSDM trigger — it can only be triggered by a DMA stream TC flag or software.

---

## Critical Configurations & Parameters

### RCC & Clocking
- **Clock Source:** `RCC_D3CCIPR` bits [23:21] (**SAI4ASEL**) must be `100` (**per_ck** / HSI 64 MHz). [RM0399, P. 598]
- **Target Frequency:** 2.0 MHz. Calculation: 64 MHz / 32 = 2.0 MHz. **MCKDIV=16** because `Total Division = MCKDIV × 2` → 16 × 2 = 32.
- Overclocking (e.g. 6.25 MHz) causes digital silence (output stuck at `−30518`).
- **Bus Clock:** `RCC_AHB4ENR` (SAI4EN) must be `1` before accessing any SAI4 registers.

### DFSDM Filter Settings
- **Channel:** `DFSDM1_Channel1` (mapped to PC1/AF6).
- **Clock Edge:** `DFSDM_CHANNEL_SPI_FALLING` (`SITP=01`). [RM0399, P. 2147]
  - LR=HIGH → mic drives data on **rising** edge → MCU samples on **falling** edge (250 ns settle buffer at 2 MHz).
- **Filter Type:** `Sinc3`.
- **Oversampling (Fosr):** **125** (16 kHz audio from 2 MHz clock: 2,000,000 / 125 = 16,000 Hz).
- **Right Bit Shift:** 5–7 bits to scale 24-bit filter result to 16-bit PCM. [RM0399, P. 2156]

### Register Fix: DFSDM_CH1CFGR1 = 0x85
- **Target Value:** `0x00000085`
- **Correction:** `SPICKSEL` = `01` (External Clock from SAI4 PE2), not `11`.

| Bits | Field | Value | Explanation |
|:-----|:------|:------|:------------|
| **1:0** | **SITP** | `01` | Falling Edge Sampling — mic drives on Rising, sample on Falling. |
| **3:2** | **SPICKSEL** | `01` | External Clock Source — latch data using SAI4 CK1 (PE2). |
| **7** | **CHEN** | `1` | Channel Enabled. |

### DMA1 Control (Current Baseline)
- **Instance:** `DMA1_Stream1`
- **Request:** `DMA_REQUEST_DFSDM1_FLT0` (DMAMUX1)
- **Direction:** Peripheral → Memory
- **Transfer Size:** 32-bit Word (both peripheral and memory alignment)
- **Mode:** **Circular** — required to keep PE2 clock running (SAI4 FIFO must never fill)
- **Buffer:** `s_DfsdmBuf[128]` at `0x30004000` (D2 SRAM1, accessible by DMA1)
- **IRQ:** `DMA1_Stream1_IRQn`, priority 5 (FreeRTOS-safe), routed via `VoiceRec_DMA_IRQHandler()`

---

## 7-Step Summary Table (SAI4 Clock Bring-up)

*Added: 16-04-2026 — Updated: 19-04-2026*

| # | Parameter | Required Value | Register / Field | Technical Reason |
|---|---|---|---|---|
| 1 | Kernel Clock Source | per_ck (HSI 64 MHz) | `RCC_D3CCIPR` → SAI4ASEL bits [23:21] = `100` | Stable 64 MHz base → exactly 2.0 MHz without PLL jitter. |
| 2 | Clock Divider | MCKDIV=16 | `SAI_ACR1` → MCKDIV | Formula: Source ÷ (MCKDIV×2). 64÷32 = 2.0 MHz. |
| 3 | Bus Clock Enable | Enabled | `RCC_AHB4ENR` → SAI4EN | SAI4 registers are invisible until AHB4 clock is gated ON. |
| 4 | GPIO Function | AF10 | `GPIOE_AFRH` → PE2 | Connects the SAI4 Clock Generator to the PE2 output pin. |
| 5 | GPIO Speed | Very High | `GPIOE_OSPEEDR` → PE2 | Prevents rounding of the 2.0 MHz square wave. |
| 6 | Peripheral Mode | Master RX | `SAI_ACR1` → MODE[1:0] | SAI4 masters the clock; RX mode required for FIFO-drain activation. |
| 7 | DMA Mode | Circular | `DMA1_Stream1` CCR → CIRC | **CRITICAL:** Prevents SAI4 FIFO from clogging. Full FIFO kills PE2 clock. |

---

## Recording Task Logic (PCM to WAV)

### State Machine
IDLE → RECORDING → SAVING

### WAV Header
- 512-byte sector-aligned header (RIFF + fmt + JUNK pad + data).
- PCM data starts at byte offset 512 — first byte of sector 1.
- All subsequent 4 KB writes land on exact sector boundaries (no RMW penalty).

### Logging Rules — MANDATORY
- **`printf` is STRICTLY FORBIDDEN in this project.** It pulls in 40 KB of semihosting machinery that spins waiting for a debugger connection and hangs the system when none is attached.
- Use **SEGGER RTT only**: `SEGGER_RTT_WriteString(0, ...)` for literals, `SEGGER_RTT_Write(0, buf, n)` for formatted strings (pre-formatted with `snprintf`).
- Use `RLOG(fmt, ...)` for timestamped RTT output via the log-queue.
- ITM (`_itm_str` / `_itm_u32`) is permitted for one-shot register snapshots at exact instruction time.
- Example: `RLOG("[REC] file=%s size=%lu bytes", filename, size);`

---

## Level 1 Register Verification (after button press)

| Register | Address | Pass Condition |
|---|---|---|
| `SAI4_Block_A->CR1` | `0x58005004` | Bit 0 (SAIEN) = **1** |
| `DMA1_Stream1->CR` | `0x40020028` | Bit 0 (EN) = **1** |
| `DFSDM1_Filter0->FLTCR1` | `0x40017100` | Bit 0 (DFEN) = **1** |
| `DFSDM1_Filter0->FLTISR` | `0x40017108` | Bit 17 (CKABF[1]) = **0** (DFSDM sees the PDM clock) |
| `RCC->D3CCIPR` bits [23:21] | `0x58024454` | = `100` (PER_CK) |
| `PWR->WKUPEPR` | `0x58024828` | Bit 5 (WKUPEN6) = **0** — PC1 not hijacked by WKUP6 |
| `s_DfsdmBuf[0]` | `0x30004000` | **Non-zero** = audio flowing |

---

## Holy Code Policy — LOCKED LINES

Any line tagged with a `/* [HOLY] */` marker (or a variant like `/* [HOLY — ...] */`) is **locked**. These lines were validated on hardware against the oscilloscope on PE2/SB45 and must not be modified without **explicit user approval AND a full scope re-verification** on the mic clock pin.

**Rules:**
1. **Do not change** the value, type, or ordering of any `[HOLY]` line — not for refactors, not for cleanup, not for "equivalent" rewrites, not even to silence a linter.
2. **Do not remove** the `[HOLY]` marker. It is the tripwire for future sessions.
3. If the user explicitly authorizes a change, you must (a) restate the full impact, (b) after editing, re-run the validation harness against PE2 (see `EWARM/runs/sai4_pe2_clock_validation_2026-04-20/README.md`), and (c) update the HOLY banner date + this policy reference.
4. Adding NEW `[HOLY]` markers requires the same level of evidence: back-to-back flash+run cycles with live DAP reads and a scope capture.

**Current HOLY regions:**
- [CM7/Core/Src/main.c](../CM7/Core/Src/main.c) — `MX_SAI4_Init` (all `hsai_BlockA4.*` fields + `RCC->D3CCIPR` MODIFY_REG).
- [CM7/Core/Src/stm32h7xx_hal_msp.c](../CM7/Core/Src/stm32h7xx_hal_msp.c) — `HAL_SAI_MspInit` (RCC periph clock config, SAI4/BDMA gates, BDMA Init fields, NVIC, and the PE2/PE5/PE4/PF8/PC1 GPIO blocks).

**Evidence trail:** [EWARM/runs/sai4_pe2_clock_validation_2026-04-20/README.md](../EWARM/runs/sai4_pe2_clock_validation_2026-04-20/README.md) — 21 flash+run cycles, 2.000 MHz square wave confirmed on SB45.
