DISABLED MACROS — backup copies of IAR device macros that were removed
======================================================================

STM32H7xx_TRACE.dmac
--------------------
Original location : C:\iar\ewarm-9.70.2\arm\config\debugger\ST\STM32H7xx_TRACE.dmac
Removed on        : 2026-03-30
Removed from      : EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM7.general.xcl
                    EWARM\settings\STM32H747I-DISCO.STM32H747I-DISCO_CM4.general.xcl
                    EWARM\settings\cspy_cm7_general.xcl

Why removed:
  The macro configures ETM/SWO hardware trace (GPIOE/GPIOB trace pins, trace
  funnel, trace clock).  The STM32H747I-DISCO J-Link connection does not have
  trace pins wired, so the macro fails at line 359 with:
    "Operation error" on __readMemory32(0xE00E4FCC, "AP2_Memory")
  The error appeared every debug session in the IAR Debug Log and was harmless
  but confusing.  We do not use ETM/SWO — we use J-Link RTT instead.

How to revert (if ETM/SWO trace is ever needed):
  Add this line back to each of the three .xcl files above:
    --device_macro=C:\iar\ewarm-9.70.2\arm/config/debugger/ST/STM32H7xx_TRACE.dmac
  Insert it after the STM32H7xx_OB.dmac line.
