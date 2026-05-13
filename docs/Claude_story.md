# The SAI4 Clock Story — Why the MDMA is the "Switch"

*Added: 16-04-2026*

---

The short answer is **No, the MDMA does not create the clock, but it is the "Switch" that turns it on.**

To understand this, you have to separate **Configuration** from **Activation**. Think of it like a water system in a house.

---

## 1. Configuration (The Plumbing) — Done by CPU

The CPU (via your initialization code) is responsible for setting up the "pipes" and the "pump."

- The CPU sets the RCC to provide **64 MHz**.
- The CPU sets the SAI4 to divide that 64 MHz down to **2.0 MHz**.
- The CPU connects **PE2** to the SAI4.

At this stage: Everything is ready, but the water (the clock) is not flowing yet. The SAI4 "valve" is closed because the internal storage (FIFO) is already full of "stale" data (zeros).

---

## 2. Activation (Opening the Valve) — Done by MDMA

The SAI4 hardware is designed to be "smart." It will **not** start the clock on PE2 until it knows there is somewhere for the data to go.

1. When you enable the MDMA, it immediately performs its first **32-bit Read** from the SAI4 data register.
2. The SAI4 hardware detects this read and realizes: *"The FIFO is no longer full; I have space to store new bits."*
3. **The Result:** The SAI4 automatically "opens the valve" and starts toggling the PE2 pin at **2.0 MHz**.

---

## 3. Sustainability — Done by MDMA Circular Mode

This is why `CLAUDE.md` emphasizes the MDMA mode setting.

- If the MDMA only reads **once** and stops, the FIFO fills up again in microseconds.
- The SAI4 sees the full FIFO and **instantly kills the clock** to prevent an error.
- By using **Circular Mode**, the MDMA is constantly "draining the sink." Because the sink never overflows, the SAI4 never feels the need to stop the clock.

---

## 4. How to Activate the MDMA

<span style="color:red">**⚠ This is Step 7 in the Summary Table — the final and most critical step. All previous steps (1–6) must be complete before this is executed.**</span>

Activating the MDMA in this pipeline is a two-step process: **Configuration** and **Software Enablement**. Because the MDMA is acting as a bridge between the D3 and D1 domains, it must be ready **before** you turn on the audio peripheral.

### Step 1 — Configuration (The Parameters)

Before calling the start function, the MDMA handle must be populated with the specific constraints of the H7 architecture:

- **Trigger Selection:** Set the trigger to the hardware event of the previous stage — the BDMA "Transfer Complete" or the DFSDM "Data Ready" signal.
- **Data Alignment:** Both `SrcDataSize` and `DestDataSize` must be **Word (32-bit)**.
- **Mode:** Set `AddressingMode` to **Circular**.

### Step 2 — Software Activation (The Command)

In the STM32 HAL library, the MDMA is activated via `HAL_MDMA_Start` or `HAL_MDMA_Start_IT`. In an audio pipeline it is linked to the peripheral start command. **Order is critical — MDMA must be started first.**

```c
// 1. Start the MDMA first so it is "listening" for the trigger
HAL_MDMA_Start(&hmdma_dfsdm_node,
               (uint32_t)&SRAM4_Buffer,  // Source: D3 RAM
               (uint32_t)&DTCM_Buffer,   // Destination: D1 RAM
               BUFFER_SIZE,
               1);

// 2. Start the DFSDM/SAI peripheral
// This fires the first DMA request, which the MDMA will catch
HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0,
                                 (int32_t *)SRAM4_Buffer,
                                 BUFFER_SIZE);
```

### Step 3 — Verification (How to Confirm it is Active)

Once the start functions are called, verify activation in IAR by checking `MDMA_CCR` for the specific channel:

- **EN Bit (Bit 0):** Must be `1`. If `0`, activation failed — likely a clock gating issue in the RCC.
- **CTCIF (Transfer Complete Flag):** In `MDMA_GISR`, this bit should be toggling or staying high if the pipeline is moving data correctly.

### Why Order Matters

If you activate the SAI4/DFSDM **before** the MDMA, the hardware FIFO fills up instantly. As described in Section 2, a full FIFO tells the hardware to kill the clock on PE2. By activating the MDMA **first**, it is standing by to perform the "First Read" the microsecond the peripheral is enabled — keeping the clock alive.

---

## Summary Table

| # | Parameter | Required Value | Register / Field | Technical Reason |
|---|---|---|---|---|
| 1 | Kernel Clock Source | per_ck (HSI 64 MHz) | RCC_D3CCIPR → SAI4ASEL (bits 21-23 = `100`) | Provides a stable 64 MHz base to derive exactly 2.0 MHz without PLL jitter. |
| 2 | Clock Divider | 16 | SAI_ACR1 → MCKDIV | Hardware formula: Source / (MCKDIV × 2). 64 / (16 × 2) = 2.0 MHz. |
| 3 | Bus Clock Enable | Enabled (1) | RCC_AHB4ENR → BDMAEN & SAI4EN | Peripherals in the D3 domain are invisible until the AHB4 bus clock is gated ON. |
| 4 | GPIO Function | AF10 | GPIOE_AFRH → PE2 | Physically connects the internal SAI4 Clock Generator to the PE2 output pin. |
| 5 | GPIO Speed | Very High | GPIOE_OSPEEDR → PE2 | Prevents rounding of the square wave at 2.0 MHz, ensuring the mic stays in sync. |
| 6 | Peripheral Mode | Master Transmit | SAI_ACR1 → MODE[1:0] | Forces the SAI4 to act as the clock master (heartbeat) for the PDM microphone. |
| 7 | DMA Mode | Circular | MDMA_CCR → CIRC | **CRITICAL:** Prevents the SAI4 FIFO from clogging. If FIFO is full, hardware kills the PE2 clock. |
