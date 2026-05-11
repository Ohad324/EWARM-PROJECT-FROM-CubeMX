# Linker Map — Snapshot

**Source:** `EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map`
**Generated:** 2026-05-11 20:23:58 by IAR ELF Linker V9.70.2.500/W64 for ARM
**Build commit:** `a303dac` (feat(lcd): re-enable LCD + video tasks alongside audio pipeline — FULL SYSTEM WORKING)

## Why this file is committed

The `.map` file is normally a build artefact and shouldn't be in version control. This snapshot is committed deliberately as a **memory-layout reference** for the milestone build where:

- LCD + audio + UART + GCS + STT all work simultaneously
- SEGGER RTT buffers moved to D3 SRAM4 (`0x38000000`)
- `rtos_trace` disabled to free 24 KB of AXI
- Full system runs without `Lp011` linker errors

Future work can diff against this file to see how AXI / D2 / D3 occupancy changes.

For the **live** map of the current build, always read [`EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map`](../EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map) on disk — it regenerates on every link.

## Key addresses at this snapshot

| Region | Range | Used |
|---|---|---|
| Flash code (`.text` + `.rodata` + fonts + assets) | `0x08000000`–`0x0805BB78` | ~370 KB |
| AXI SRAM (D1) | `0x24000000`–`0x2407FFFF` | ~498 KB (incl. 281 KB framebuffer + 96 KB heap) |
| DTCM (D1) | `0x20000000`–`0x20002000` | 12 KB (CSTACK 8 KB + HEAP 4 KB) |
| D2 SRAM1 | `0x30000000`–`0x30004200` | 512 B (`s_DfsdmBuf` only) |
| D2 SRAM2 | `0x30020000`–`0x30037700` | 96 KB (`g_AudioBuf`) |
| **D3 SRAM4** (NEW) | `0x38000000`–`0x38004000` | ~16 KB (RTT buffers + control block) |
| External SDRAM | `0xD0000000`–`0xD0589800` | ~5.4 MB (JPEG decode intermediates) |

## Full map dump

```
###############################################################################
#
# IAR ELF Linker V9.70.2.500/W64 for ARM                  11/May/2026  20:23:58
# Copyright 2007-2025 IAR Systems AB.
#
#    Output file  =
#        C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\Exe\STM32H747I-DISCO_CM7.out
#    Map file     =
#        C:\TouchGFXProjects\MyApplication\EWARM\STM32H747I-DISCO_CM7\List\STM32H747I-DISCO_CM7.map
#
###############################################################################
```

The complete map is preserved as a sibling file: [`Map_full.txt`](Map_full.txt) (rendered as plain text for grep-friendliness and to avoid bloating this markdown).

For a structured audit per symbol, see [MEMORY_MAP_AND_SW_BLOCKS.md](MEMORY_MAP_AND_SW_BLOCKS.md).
