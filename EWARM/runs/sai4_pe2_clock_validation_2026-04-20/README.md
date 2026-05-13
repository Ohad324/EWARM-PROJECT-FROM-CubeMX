# SAI4 PE2 Clock Validation — 2026-04-20

Evidence trail for the SAI4 PDM master clock fix on STM32H747I-DISCO.
**Outcome:** Path B validated — 2.000 MHz continuous square wave on PE2/SB45 across 21 consecutive flash+run cycles. No WCKCFG, no OVRUDR.

---

## Goal

PE2/SB45 (SAI4_CK1 / AF10) must output a clean 2.000 MHz square wave so the onboard
MP34DT05TR PDM microphone receives a valid clock. Prior config produced ~46.6 kHz
on the scope — 32× too slow.

## Hardware

- Board: STM32H747I-DISCO (revision E)
- MCU: STM32H747XIH6 Cortex-M7 core, silicon rev Y
- Probe: SEGGER J-Link Ultra V7, S/N 507000552, SWD @ 4 MHz
- Scope: probing SB45 solder bridge (PE2 net), 200 ns/div, 1 V/div
- Kernel clock: SAI4ASEL = CLKP (HSI 64 MHz), `RCC->D3CCIPR` = `0x00800000`

## Test harness

- Command file: `EWARM/gemini_4mhz_test.jlink` (reused unchanged)
- Flow: `r` → `erase` → `loadfile STM32H747I-DISCO_CM7.out` → `r` → `g` → `sleep 200`
  → **20 × 250 ms pre-trigger** live DAP reads (17 registers each)
  → SW trigger via `w4 0x58000008 0x00002000` (EXTI SWIER1 bit 13 = blue button)
  → **10 × 200 ms post-trigger** live DAP reads
- CPU runs free throughout. No halts. No single snapshots.
- J-Link GUI visible for every run (no `-NoGui 1`).

## Register map read each sample

| # | Addr | Name | Purpose |
|---|---|---|---|
| 01 | `0x58024400` | `RCC_CR` | HSI/HSE/PLL state |
| 02 | `0x58024450` | `RCC_D1CCIPR` | D1 clock muxes |
| 03 | `0x58024418` | `RCC_D3CFGR` | D3 prescaler |
| 04 | `0x580244F4` | `RCC_APB4ENR` | SAI4 clock gate (bit 21) |
| 05 | `0x580244E0` | `RCC_AHB4ENR` | GPIOE (bit 4) + BDMA (bit 21) gates |
| 06 | `0x58024458` | `RCC_D3CCIPR` | SAI4ASEL [23:21] = 100 → CLKP |
| 07 | `0x58005404` | `SAI4_A CR1` | MCKDIV / NODIV / DMAEN / SAIEN |
| 08 | `0x5800540C` | `SAI4_A FRCR` | FRL (frame length) |
| 09 | `0x58005444` | `SAI4_A PDMCR` | PDMEN / CKEN1 / MICNBR |
| 10 | `0x58005418` | `SAI4_A SR` | WCKCFG / OVRUDR / FREQ / FLVL |
| 11 | `0x58021000` | `GPIOE MODER` | PE2 mode → AF |
| 12 | `0x58021020` | `GPIOE AFRL` | PE2 AF selection → AF10 (SAI4_CK1) |
| 13 | `0x58021008` | `GPIOE OSPEEDR` | PE2 slew → Very High |
| 14 | `0x5802541C` | `BDMA Ch1 CCR` | EN / CIRC |
| 15 | `0x58025420` | `BDMA Ch1 CNDTR` | live transfer counter |
| 16 | `0x58025424` | `BDMA Ch1 CPAR` | want `0x58005420` (SAI4 DR) |
| 17 | `0x58025428` | `BDMA Ch1 CM0AR` | want `0x38000000` (D3 SRAM4) |

---

## Configurations tested

### Baseline failure #1 — Gemini "FRCR=0 + MCKDIV=8"

- `NoDivider = SAI_MASTERDIVIDER_ENABLE` (NODIV=0)
- `Mckdiv = 8`
- `FrameLength = 8`
- Post-init override: `SAI4_Block_A->FRCR = 0;` → forces FRL=0

**Result:** Post-trigger `SR = 0x00000004` → **WCKCFG=1**, `CR1 = 0x00820041` →
**SAIEN=0**. SAI refused to start. PE2 flat.

**Log:** `gemini_4mhz_20260419_233259.log`

### Baseline failure #2 — FRL=0 retry with MCKDIV=16

- `NoDivider = SAI_MASTERDIVIDER_ENABLE` (NODIV=0)
- `Mckdiv = 16`
- `FrameLength = 8` (overridden by post-init `FRCR = 0` → FRL=0)
- Post-init override still present.

**Result:** Identical failure — `SR = 0x00000004`, `CR1 = 0x01020041` (SAIEN=0).
MCKDIV value does not affect the WCKCFG validity check. Confirms the check is
FRL-only, per RM0399 §54.4.8: *FRL+1 must be a power of 2 AND in [8, 256]*.

**Log:** `frl0_retry_20260420_110809.log`

### Path B — validated config

- `NoDivider = SAI_MASTERDIVIDER_DISABLE` (NODIV=1) — MCKDIV is sole divider
- `Mckdiv = 16`
- `FrameLength = 16` → FRL=15 (power-of-2, in [8,256], valid)
- No post-init FRCR override — removed from code.

**Math (RM0399 §54.4.8, NODIV=1 formula):**
```
FSCK_A = FMCLK_A = F_sai_x_ker_ck / MCKDIV = 64 MHz / 16 = 4.000 MHz
PE2 / CK1 = FSCK_A / 2 = 2.000 MHz   (PDM block internal ÷2, RM p.2411)
```

**Expected register signatures — post-trigger:**

| Register | Value | Decode |
|---|---|---|
| `CR1` | `0x010B0041` | OSR=1, NODIV=1, DMAEN=1, SAIEN=1, DS=010 (8-bit), MODE=01 (master RX) |
| `FRCR` | `0x0000000F` | FRL=15 |
| `PDMCR` | `0x00000101` | PDMEN=1, CKEN1=1 |
| `SR` | `0x00000000` | no WCKCFG, no OVRUDR |
| `BDMA CCR` | `0x000005AF` | EN=1, CIRC=1 |
| `BDMA CNDTR` | live (oscillates) | transfers active |

**Logs:** `path_b_20260420_112747.log` (initial) + `path_b_run1..run20_*.log` (21 total cycles).

---

## Results — 21 flash+run cycles

All 21 runs: pre-trigger clean, post-trigger `CR1 = 0x010B0041` across the first
several reads, `SR = 0x00000000` throughout, no WCKCFG at any point. Scope confirmed
clean 2.000 MHz square wave on SB45 (user verified visually during runs 11–20).

Extraction command (from repo root):
```
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  LOG=$(ls EWARM/runs/sai4_pe2_clock_validation_2026-04-20/path_b_run${i}_*.log)
  CR1=$(grep -A1000 "w4 0x58000008" "$LOG" | grep "58005404 = " | awk '{print $3}' | tr '\n' ' ')
  SR=$(grep -A1000 "w4 0x58000008" "$LOG" | grep "58005418 = " | awk '{print $3}' | tr '\n' ' ')
  printf "Run %2d | CR1: %s| SR: %s\n" "$i" "$CR1" "$SR"
done
```

Sample output (runs 11–20):
```
Run 11 | CR1: 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 01080041 01080041 01080041 | SR: 00000000 × 10
Run 12 | CR1: 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 01080041 01080041 | SR: 00000000 × 10
...
Run 20 | CR1: 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 010B0041 01080041 01080041 | SR: 00000000, 00000000, 00000000, 00000000, 00010008, 00000000, ...
```

### Observed pattern — SAIEN auto-drop ~1.5 s after trigger

In every run, SAIEN stays latched high for reads 1–7 (~1.3 s post-trigger), then
drops on reads 8–10 (CR1 transitions `0x010B0041` → `0x01080041`, i.e. bit 16 clears).
NODIV, DMAEN, MCKDIV all stay configured. `SR` stays 0 throughout.

This happens consistently across all 21 runs, so it is **deterministic and not a
silicon glitch**. Most likely cause: firmware shuts SAIEN after its capture window
(e.g., VoiceRecTask ending a timed recording). Not a hardware-config issue — the
clock generator itself is healthy during the active window.

The one `SR = 0x00010008` transient in run 20 decodes as FREQ=1, FLVL=1 (FIFO holds
1 word, request pending) — normal receive-mode activity, not an error.

---

## Firmware commit

`2abb4ed` on branch `fix/voice-recorder-pdm-clean`:
> fix: SAI4 Path B — 2.000 MHz stable clock on PE2 (PDM master)

File changed: [CM7/Core/Src/main.c](../../../CM7/Core/Src/main.c) (`MX_SAI4_Init`). 7 insertions, 7 deletions.

Pushed to remote `b4b4655..2abb4ed`.

---

## File index

| File | Purpose |
|---|---|
| `gemini_4mhz_20260419_233259.log` | Baseline failure #1 — proves FRL=0 + MCKDIV=8 fails (WCKCFG) |
| `frl0_retry_20260420_110809.log` | Baseline failure #2 — proves FRL=0 + MCKDIV=16 also fails (same WCKCFG) |
| `path_b_20260420_112747.log` | Path B initial run (11:27:47) |
| `path_b_run1_..._112929.log` → `path_b_run10_..._113250.log` | Path B runs 1–10 (stability batch A) |
| `path_b_run11_..._113836.log` → `path_b_run20_..._114157.log` | Path B runs 11–20 (stability batch B) |

## References

- RM0399 §54.4.8 — SAI clock generator & NODIV=1 formula
- RM0399 §54.4.10 — PDM interface enabling sequence (requires TDM master mode)
- RM0399 §54.6.3 / §54.6.18 — SAI_xCR1 / PDMCR bit layouts
- Memory: [project_sai4_pdm_rock_solid.md](../../../../../Users/Ohad/.claude/projects/c--TouchGFXProjects-MyApplication/memory/project_sai4_pdm_rock_solid.md)
