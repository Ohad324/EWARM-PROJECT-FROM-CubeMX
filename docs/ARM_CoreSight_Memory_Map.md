# ARM Cortex-M CoreSight Memory Map
### Reference for STM32H747 (Cortex-M7 core)

---

## Overview

The CoreSight debug & trace region occupies the top of the ARM Cortex-M fixed
memory map at `0xE0000000–0xE00FFFFF` (1 MB, vendor-independent).  
Every Cortex-M device (M0 through M7) has these blocks at identical addresses.  
None of this region consumes your RAM or Flash — it is hardwired silicon.

---

## Block Map

```
┌─────────────────────────────────────────────────────────────────────┐
│  Address Range            Block    Full Name                        │
├─────────────────────────────────────────────────────────────────────┤
│  0xE0000000 – 0xE0000FFF  ITM      Instrumentation Trace Macrocell  │
│  0xE0001000 – 0xE0001FFF  DWT      Data Watchpoint & Trace          │
│  0xE0002000 – 0xE0002FFF  FPB      Flash Patch & Breakpoint         │
│  0xE0003000 – 0xE000CFFF  —        Reserved                         │
│  0xE000D000 – 0xE000DFFF  —        Reserved                         │
│  0xE000E000 – 0xE000EFFF  SCS      System Control Space             │
│                             ├─ SysTick   0xE000E010                 │
│                             ├─ NVIC      0xE000E100                 │
│                             ├─ SCB       0xE000ED00                 │
│                             └─ MPU       0xE000ED90                 │
│  0xE000F000 – 0xE000FFFF  —        Reserved                         │
│  0xE0040000 – 0xE0040FFF  TPIU     Trace Port Interface Unit (SWO)  │
│  0xE0041000 – 0xE0041FFF  ETM      Embedded Trace Macrocell         │
│  0xE0042000 – 0xE00FEFFF  —        Reserved                         │
│  0xE00FF000 – 0xE00FFFFF  ROM      ROM Table (debugger discovery)   │
└─────────────────────────────────────────────────────────────────────┘
```

---

## ITM — Instrumentation Trace Macrocell  `0xE0000000`

Used for software-generated trace events (our `ITM_STAGE()` macro).

```
Offset   Register        Description
──────────────────────────────────────────────────────────────────────
0x000    PORT[0]         Stimulus port 0  (u8 / u16 / u32 write)
0x004    PORT[1]         Stimulus port 1  ← our pipeline tokens
0x008    PORT[2]         Stimulus port 2
  ...      ...           (32 ports total, 4 bytes each)
0x07C    PORT[31]        Stimulus port 31
0xE00    TER             Trace Enable Register (1 bit per port)
0xE40    TPR             Trace Privilege Register
0xE80    TCR             Trace Control Register (global enable)
0xFB0    LAR             Lock Access Register
0xFB4    LSR             Lock Status Register
0xFD0    PID4            Peripheral ID4
  ...      ...
0xFF8    CID3            Component ID3
```

**Key rule:** Before writing to a stimulus port, always check  
`ITM->PORT[n].u32 != 0` — returns 0 when no debugger is attached  
(write is silently discarded = safe in release builds).

**Our usage:**
- `PORT[0]` → `STAGE_CYCLES()` char-by-char timing strings
- `PORT[1]` → `ITM_STAGE(token)` pipeline tokens (single 32-bit word, 7 cycles)

---

## DWT — Data Watchpoint & Trace  `0xE0001000`

Used for hardware watchpoints and the free-running cycle counter.

```
Offset   Register        Description
──────────────────────────────────────────────────────────────────────
0x000    CTRL            Control (CYCCNTENA bit 0 = enable cycle count)
0x004    CYCCNT          Cycle Count Register ← used by STAGE_CYCLES()
0x008    CPICNT          CPI Count Register
0x00C    EXCCNT          Exception Overhead Count Register
0x010    SLEEPCNT        Sleep Count Register
0x014    LSUCNT          LSU Count Register
0x018    FOLDCNT         Folded Instruction Count Register
0x01C    PCSR            Program Counter Sample Register
0x020    COMP0           Comparator 0 (watchpoint address)
0x024    MASK0           Comparator 0 Mask
0x028    FUNCTION0       Comparator 0 Function
  ...      ...           (up to 4 comparators on Cortex-M7)
```

**Our usage:** `DWT->CYCCNT` read inside `STAGE_CYCLES()` to timestamp events.  
Enable with: `DWT->CTRL |= 1` (after `CoreDebug->DEMCR |= TRCENA`).

---

## FPB — Flash Patch & Breakpoint  `0xE0002000`

Provides up to 8 hardware breakpoints and flash patching.  
Used automatically by the IAR debugger — not accessed directly in firmware.

```
Offset   Register        Description
──────────────────────────────────────────────────────────────────────
0x000    CTRL            Control (NUM_CODE = breakpoint count)
0x004    REMAP           Remap base address for flash patching
0x008    COMP0           Comparator 0 (breakpoint address)
  ...      ...
0x020    COMP6           Comparator 6
```

---

## SCS — System Control Space  `0xE000E000`

The most-used CoreSight block in normal firmware.

```
Offset   Block      Key Registers
──────────────────────────────────────────────────────────────────────
0x010    SysTick    CTRL, LOAD, VAL, CALIB
0x100    NVIC       ISER[0..7]  (interrupt set-enable)
                    ICER[0..7]  (interrupt clear-enable)
                    ISPR[0..7]  (interrupt set-pending)
                    IPR[0..59]  (interrupt priority)
0xD00    SCB        CPUID, ICSR, VTOR, AIRCR, SCR, CCR
                    CFSR  ← HardFault / UsageFault / BusFault flags
                    HFSR  ← HardFault status
                    MMFAR ← MemManage fault address
                    BFAR  ← BusFault address
0xD90    MPU        TYPE, CTRL, RNR, RBAR, RASR
0xF00    STIR       Software Trigger Interrupt Register
```

**CFSR at `0xE000ED28`** is the first register to read on any HardFault —  
it breaks down into UFSR (usage), BFSR (bus), MMFSR (memory) fault bits.

---

## TPIU — Trace Port Interface Unit  `0xE0040000`

The hardware serializer that puts ITM packets onto the **SWO pin**.  
Configured by IAR/J-Link at debug session start — not accessed in firmware.

```
Offset   Register        Description
──────────────────────────────────────────────────────────────────────
0x000    SSPSR           Supported Port Sizes
0x004    CSPSR           Current Port Size
0x010    ACPR            Async Clock Prescaler (sets SWO baud rate)
0x0F0    SPPR            Selected Pin Protocol (0=TracePort, 2=NRZ/UART)
0xFB0    LAR             Lock Access Register
```

**SWO baud rate** = CPU clock / (ACPR + 1).  
On STM32H747 @ 480 MHz with ACPR=47: SWO = 10 Mbit/s.

---

## ETM — Embedded Trace Macrocell  `0xE0041000`

Provides full instruction trace (every executed instruction).  
Requires a 4-bit parallel trace port (not available on DISCO board — SWO only).  
Not used in this project.

---

## ROM Table  `0xE00FF000`

A read-only table the J-Link/debugger reads at connect time to discover  
which CoreSight components are present and their offsets.  
Not accessed in firmware.

---

## C Header Mapping (`core_cm7.h`)

| Block    | C macro    | Type          | Base address  |
|----------|------------|---------------|---------------|
| ITM      | `ITM`      | `ITM_Type *`  | `0xE0000000`  |
| DWT      | `DWT`      | `DWT_Type *`  | `0xE0001000`  |
| FPB      | `FPB`      | `FPB_Type *`  | `0xE0002000`  |
| SysTick  | `SysTick`  | `SysTick_Type *` | `0xE000E010` |
| NVIC     | `NVIC`     | `NVIC_Type *` | `0xE000E100`  |
| SCB      | `SCB`      | `SCB_Type *`  | `0xE000ED00`  |
| CoreDebug| `CoreDebug`| `CoreDebug_Type *` | `0xE000EDF0` |

All are pointer-cast macros — zero RAM cost, compile-time address resolution.

---

## Our Project Usage Summary

| Macro / Call         | Block | Address      | Cost         |
|----------------------|-------|--------------|--------------|
| `ITM_STAGE(token)`   | ITM   | `0xE0000004` | **7 cycles** |
| `STAGE_CYCLES(label)`| ITM + DWT | `0xE0000000` + `0xE0001004` | ~N×7 cycles (char loop) |
| `DWT->CYCCNT`        | DWT   | `0xE0001004` | 2 cycles     |
| `SCB->CFSR`          | SCB   | `0xE000ED28` | 2 cycles     |
| NVIC priority regs   | NVIC  | `0xE000E400` | 2 cycles     |

---

*ARM Architecture Reference Manual — ARMv7-M (DDI0403)*  
*ARM Cortex-M7 Technical Reference Manual (DDI0489)*  
*CoreSight Architecture Specification (IHI0029)*
