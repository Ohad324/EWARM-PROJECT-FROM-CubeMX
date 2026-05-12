# Memory Architecture, From The Trenches — STM32H747 + TouchGFX + FreeRTOS + Audio

*A practical write-up from a real embedded project where memory placement decisions determined whether the system worked at all.*

*Written for embedded software and hardware engineers working with STMicroelectronics parts — particularly those who have built (or are about to build) anything more complex than a blinky on an H7.*

---

## 1. Introduction

This post is a retrospective on the memory-architecture work I did building a voice-controlled music player on an **STM32H747I-DISCO** board. The system records audio from an onboard PDM microphone, streams it over UART to an ESP32 (NORA-W106) which uploads to Google Cloud Storage, calls Google Speech-to-Text, and routes the transcript back. In parallel, a TouchGFX UI runs on the 800×480 DSI panel, with a partial-framebuffer (PFB) rendering pipeline. All of this is coordinated by FreeRTOS.

I built the firmware in **IAR Embedded Workbench 9.70.2** with `cspybat` for command-line debugging via SEGGER J-Link.

You don't get a system like this working without taking memory placement seriously. The STM32H747 has **three independent power domains** with **multiple SRAM regions** that are not freely interchangeable — each DMA controller can only reach certain regions, each region has different cache behaviour, and the linker's default placement is wrong as often as it is right. Picking the correct domain for each buffer is the difference between "the screen renders" and "the screen tears every frame".

What you'll learn from this post: the H7's three-domain layout and the engineering reasons ST chose it; how the AHB bus matrix and MDMA tie the domains together; a concrete block diagram of one real project's tasks and buffers; how to read a linker `.map` to verify placement decisions; and a short rule book I now apply to every H7 project. Every example is from real firmware and every decision is traceable back to a `.map` file.

---

## 2. STM32H7 Memory Domain Overview

The H747 silicon is partitioned into three power domains. Each owns its own clock gate, voltage regulator, can sleep independently, and contains its own SRAM and peripherals.

```
+-----------------------------------------------------------------------+
|  D1 — CORE DOMAIN  (always-on while M7 runs)                         |
|  CPU:  Cortex-M7 @ up to 480 MHz                                      |
|  DMA:  MDMA (cross-domain), DMA2D (graphics, D1-only)                 |
|  RAM:                                                                 |
|    Flash       0x08000000   1 MB    code + read-only data             |
|    ITCM        0x00000000   64 KB   M7 instruction-side, no DMA reach |
|    DTCM        0x20000000   128 KB  M7 data-side, no DMA reach        |
|    AXI SRAM    0x24000000   512 KB  DMA1/DMA2/MDMA accessible         |
+-----------------------------------------------------------------------+
                          | AHB bus matrix crossbar
+-----------------------------------------------------------------------+
|  D2 — PERIPHERAL DOMAIN                                               |
|  CPU:  Cortex-M4 @ up to 240 MHz (unused on this project)             |
|  DMA:  DMA1 (audio path), DMA2 (SDMMC bounce, generic)                |
|  Peripherals: DFSDM1, SDMMC1/2, SAI1-3, SPI, I2C, USART, USB, UART8   |
|  RAM:                                                                 |
|    SRAM1       0x30000000   128 KB  DMA1/DMA2 reachable               |
|    SRAM2       0x30020000   128 KB  DMA1/DMA2 reachable               |
|    SRAM3       0x30040000   32 KB   DMA1/DMA2 reachable               |
+-----------------------------------------------------------------------+
                          | silicon bridges where they exist
+-----------------------------------------------------------------------+
|  D3 — LOW-POWER DOMAIN  (can stay alive while D1+D2 are halted)       |
|  CPU:  none                                                           |
|  DMA:  BDMA (D3-local only)                                           |
|  Peripherals: SAI4, LPTIM, RTC, LPUART, ADC3, DAC, I2C4, SPI6         |
|  RAM:                                                                 |
|    SRAM4       0x38000000   64 KB                                     |
|    Backup      0x38800000   4 KB    battery-backed across power loss  |
+-----------------------------------------------------------------------+
```

The names get easier once you stop reading "D1, D2, D3" as ordinal and start reading them as **purpose**: **D1 is for processing**, **D2 is for peripherals**, **D3 is for whatever has to stay alive when most of the chip is asleep**. Everything else falls out of that.

---

## 3. Bus Matrix and Domain Placement

When working with the STM32H7, the primary architectural logic to consider is the distributed memory and bus design. Unlike simpler MCUs where memory is a single uniform block, the H7 is split into distinct domains (D1, D2, D3) connected by a complex bus matrix. To prevent system stalls or jitter, a developer must surgically place data based on its consumer. High-speed application code and graphics should live in D1-domain AXI SRAM for maximum CPU performance. Peripheral data — such as UART or SAI buffers — should ideally reside in D2-domain SRAM1/2, allowing DMA controllers to move data without competing for the CPU's local buses. Ignoring this spatial logic leads to bus contention, where a high-speed peripheral can effectively starve the CPU, causing real-time deadlocks often seen in complex multitasking projects.

### Why D3 is the low-power domain

Imagine the chip in deep sleep. The user has put the device down, the screen is off, the M7 is halted. You still want the device to wake on a specific event — a button press, a wake-word, a real-time clock alarm, an audio level rising above threshold. You don't want to run a 480 MHz Cortex-M7 to do that, because it would drain the battery in hours.

ST's answer is D3. Its peripherals are exactly the ones useful for "listen for something while everything else sleeps": RTC for time-of-day wakeups, LPUART and LPTIM for low-power comms and timing, SAI4 for audio input, ADC3 for analog sampling. Its DMA controller (BDMA) is small, sips power, and can only access its own SRAM4 — that's not a limitation but a design constraint that lets ST keep BDMA's clock tree small enough to leave running while the rest of the chip is off. When the wake event happens, D3 kicks D1 and D2 back to life and hands the data off. On this project D3 sits unused most of the time, but the architectural option is there.

### Why D2 is the peripheral domain

D2 is where the workhorses live: serial buses, audio interfaces, SD card, USB, Ethernet. These peripherals need DMA to keep up with sustained throughput — moving an audio sample every 62.5 µs, an SD card sector every few microseconds, an Ethernet frame every microsecond at line rate. The CPU can't be in that loop; DMA1 and DMA2 are.

ST gave D2 its own SRAM (SRAM1/2/3, totalling 288 KB) so that peripheral DMA can complete without ever touching the D1 AXI bus. That keeps the M7 free to run application code in parallel, and avoids contention between (say) the LCD scanning out pixels from AXI and DMA1 dropping audio samples into SRAM1. The peripheral domain can also wake the chip more autonomously than D1 — useful for "USB inserted" or "Ethernet packet arrived" wake patterns.

### Why D1 is the core domain

D1 is the M7 itself: ITCM for instructions, DTCM for stack/data, and AXI SRAM as the main working RAM. AXI is reachable by every DMA master in the system, which is exactly what you want for buffers that multiple subsystems touch — like a framebuffer that the CPU writes, DMA2D blits into, and LTDC scans out of. Putting AXI in D1 means it inherits the M7's high-speed clock. Putting the framebuffer in D1 AXI rather than external SDRAM removes one major source of FUIF (LCD FIFO underrun): the LCD controller doesn't have to fight the SDRAM controller's refresh cycles for bus access.

### The AHB bus matrix — what stitches it all together

The H7's bus matrix is a crossbar switch. Each **master** (CPU AXI port, M4 instruction/data ports, DMA1, DMA2, BDMA, MDMA, DMA2D, LTDC, SDMMC IDMA, JPEG codec, etc.) has a connection point. Each **slave** (every SRAM region, every peripheral register block, FMC, QSPI) has another. The matrix routes any master to any reachable slave, with hardware arbitration when two masters want the same slave at the same time.

The key practical fact: **not every master can reach every slave**. DMA1/2 live in D2 and can reach D1 AXI plus all D2 SRAMs, but not D3 SRAM4. BDMA lives in D3 and can only reach D3 SRAM4. MDMA is the exception — it has connections into every domain — which is why it's the right tool when you need to move data between domains. If you put a DFSDM DMA buffer in D3 SRAM4 expecting DMA1 to fill it, the build will compile, the link will succeed, the chip will boot, and the buffer will just stay zero forever because DMA1 cannot reach that address. There is no error, no warning, no diagnostic. That's the kind of bug placement awareness prevents.

### MDMA — the special DMA

MDMA is worth its own paragraph. Unlike DMA1, DMA2 and BDMA, which are tied to specific peripheral request lines and live within a single domain, **MDMA is a memory-to-memory engine that can read from any source in any domain and write to any destination in any other**. It's designed for bulk transfers — copying decoded JPEG frames from SDRAM into AXI, copying audio blocks from D2 SRAM into D1 AXI for cache-coherent processing, copying flash assets into RAM during boot.

MDMA has more sophisticated channel features than DMA1/2: it can do 2D transfers (useful for image regions), endian conversion on the fly, and complex linked-list descriptors. It's the right hammer when "copy big block, don't care which CPU or DMA touches it" is what you need. The trade-off is higher per-transfer setup cost, so it's not the right choice for moving 16 bytes between adjacent SRAMs every millisecond — for that, DMA1/2 are leaner.

---

## 4. SW Component Block Diagram

Here's how the moving parts of this specific project relate. The audio path lives entirely in D2. The UART RX queue is in D1 AXI. RTT diagnostic buffers live in D3 SRAM4. LCD framebuffer is in D1 AXI. Tasks have priorities that let `VoiceRecTask` (32) preempt almost everything, with `UARTReceiveTask` (26) close behind, and `TouchGFXTask` (High, ~24) rendering the UI in the background.

```
+---------------------+               +---------------------+
|  MP34DT05-A PDM mic |               |  NORA-W106 ESP32    |
|  CLK on PC2 (2MHz)  |               |  UART8 @ 921600     |
|  Data on PC1        |               |  WiFi -> GCS/STT    |
+---------+-----------+               +---------+-----------+
          | PDM bitstream                       | UART8 RX/TX
          v                                     v
+---------------------+               +---------------------+
| DFSDM1 (D2)         |               | UART8 + DMA1 Str.0  |
| Sinc3, OSR=125      |               | DMA-IDLE pattern    |
| -> 16 kHz PCM       |               +---------+-----------+
+---------+-----------+                         |
          | DMA1 Stream 1                       | ISR callback
          v                                     v
+---------------------+               +---------------------+
| s_DfsdmBuf @ D2     |               | xRawBleQueue        |
| 0x30004000, 512 B   |               | static, in D1 AXI   |
| Non-Cacheable/Share |               | StaticQueue_t + buf |
+---------+-----------+               +---------+-----------+
          | DMA HT/TC ISR                       | xQueueReceive
          v                                     v
+---------------------+               +---------------------+
| StoreDmaChunk       |               | UARTReceiveTask     |
| CPU shift+pack >>8  |               | priority 26         |
+---------+-----------+               +---------+-----------+
          | int16 PCM                           |
          v                                     v
+---------------------+               +---------------------+
| g_AudioBuf @ D2     |               | route ASCII lines, |
| 0x30020000, 96 KB   |               | match AUDIO:READY  |
+---------+-----------+               +---------+-----------+
          v                                     v
+---------------------+        +---->+---------------------+
| VoiceRecTask        |        |     | AudioSD_NotifyReady |
| priority 32         |        |     +---------+-----------+
+---------+-----------+        |               |
          v                    |               v
+---------------------------------------------------+
| SDWriteTask  (priority 20, stack 2048W in AXI)    |
|  1. write WAV header + PCM to SD card             |
|  2. send "AUDIO:FILE name SIZE" via UART8 TX      |
|  3. wait for AUDIO:READY from NORA                |
|  4. stream 96 KB PCM over UART8 TX                |
+---------------------------------------------------+

In parallel:                          Diagnostics:
+---------------------+               +---------------------+
| TouchGFXTask        |               | SEGGER RTT          |
| priority High       |               | buffer in D3 SRAM4  |
| Partial framebuffer |               | 0x38000000          |
| 4 strips x 120 rows |               | viewable real-time  |
| FB at 0x24000000    |               | via J-Link          |
| (D1 AXI)            |               +---------------------+
+---------------------+
```

Three things are worth pointing out. **One:** the audio path lives entirely in D2 — DFSDM (D2 peripheral), DMA1 (D2 DMA), `s_DfsdmBuf` (D2 SRAM1), `g_AudioBuf` (D2 SRAM2). Zero AXI bus involvement during recording, so the LCD scanning AXI doesn't interfere with audio capture and vice versa. **Two:** the UART RX queue is in D1 AXI — both producer (ISR) and consumer (`UARTReceiveTask`) are CPU code, never DMA, so the queue doesn't need to be in a DMA-reachable region. AXI is fine and sits next to the DMA buffer (`s_dma_rx_buf`) the ISR reads from. **Three:** RTT buffers live in D3 SRAM4 — not because of any hard requirement (the M7 reaches D3 over the bus matrix without issue) but because AXI was already 97% full and SRAM4 was completely unused. Section attribution makes this kind of relocation a single-line change.

---

## 5. Memory Placement Audit — Reading the `.map`

Understanding the STM32H747 memory architecture is essential before assigning any buffer or RTOS object to a region. The `.map` file in IAR is the final, comprehensive report generated by the linker after each build, and it is the **source of truth** for the physical location of every function and variable in the MCU's memory. While the `.icf` file acts as the blueprint or plan, the `.map` file shows the actual execution: it details exactly which address was assigned to every symbol, its size, and which memory section it belongs to.

For an STM32H7 developer, this is the most essential tool for verifying that placement was successful. It allows you to confirm that DMA buffers are sitting in SRAM1 (and not accidentally in DTCM, which is inaccessible to DMA), or to ensure that your static queues are properly located in the AXI SRAM as intended. Analysing the map file is crucial for identifying memory hogs, preventing overflow, and resolving complex bus contention issues by verifying a total physical separation between different system variables.

Each entry in the `.map` looks like:

```
s_DfsdmBuf              0x30004000      0x200  Data  Lc  voice_recorder.o [5]
                        ^address        ^size              ^object file
```

So `s_DfsdmBuf` is 512 bytes (`0x200`) at `0x30004000` — inside D2 SRAM1. That matches the `#pragma location = 0x30004000` directive in `voice_recorder.c`.

### How the IAR `.map` file helps during development

The IAR `.map` file is more than a passive report — it's the developer's most reliable debugging document. On every build, the linker writes a complete inventory of the firmware to `EWARM/<Config>/List/<Project>.map`. This includes the absolute address and size of every symbol, the section each symbol was placed into, the object file it came from, and a region-by-region summary of free space. Three habits make it indispensable on this project. **First**, grepping for a specific symbol immediately tells you whether the linker honoured your placement intent — if you wrote `#pragma location = ".sram1"` and the symbol appears at `0x24050000`, you know the section directive was wrong or missing in the `.icf`. **Second**, the placement summary at the top of the file shows total bytes used per region, which lets you see how close you are to a `Lp011: section placement failed` error before it actually fires. **Third**, diffing the `.map` between two builds is the fastest way to catch unintended growth — a refactor that adds 25 KB to AXI `.bss` will show up as a region-summary delta even if no code change "looked" memory-related. On this project, the `.map` was the diagnostic that caught the LCD re-enable overflow, the rtos_trace dead-storage cost, and the RTT-buffer-in-AXI inefficiency that we eventually moved to D3 SRAM4.

### The `#pragma location` directive

`#pragma location` is an IAR-specific compiler instruction used to force the placement of a variable or function at a specific memory address or within a named memory section defined in the `.icf` file. Unlike standard variable declarations where the compiler chooses the location automatically, this directive gives the developer precise control over the hardware mapping. In high-performance systems like the STM32H7, it is a critical tool for ensuring that DMA buffers are placed in peripheral-accessible RAM (like SRAM1) and that high-frequency RTOS objects are stored in fast-access memory (like AXI SRAM). By using this pragma, you move away from the unpredictability of a dynamic heap and toward a deterministic, hard-coded memory layout that prevents bus contention and ensures system stability.

### How `#pragma location` was used in this project

Five buffers in particular benefited from explicit placement. `s_DfsdmBuf` got `#pragma location = 0x30004000` because the DMA controller has to write there and the surrounding 127.5 KB of D2 SRAM1 must remain empty as headroom for future audio extensions; using an absolute address lets the linker treat the surrounding area as reservable space. `g_AudioBuf` got `#pragma location = 0x30020000` placing it at the start of D2 SRAM2 — same reasoning, but in a different SRAM. `s_dma_rx_buf`, `s_rawBleQueueCB`, and `s_rawBleQueueStorage` got `#pragma location = ".axi_sram"` (a named section defined in the `.icf`) so they're grouped together in D1 AXI alongside their consumer task. The SEGGER RTT control block and ring buffers got `#pragma location = ".sram4_rtt"`, which is how we moved 16 KB of diagnostic buffer out of AXI without changing a single line of SEGGER's library code. In every case the goal is the same: place the buffer next to the master that drives it, document the choice with a one-line comment naming the domain and reason, and then verify in the `.map` that the linker honoured the request. This is the practical mechanism that converts the "spatial logic" of the bus matrix from abstract architectural rule into compile-time enforcement.

### What ended up where (the audit by domain)

**D1 DTCM (`0x20000000`, 128 KB)** — `CSTACK` (8 KB) and the unused C-runtime `HEAP` (4 KB). No DMA can reach DTCM; the M7 stack is safe by construction. ~116 KB free.

**D1 AXI SRAM (`0x24000000`, 512 KB)** — `TouchGFX_Framebuffer` at the start (281 KB), `ucHeap` for FreeRTOS (96 KB), HAL handles, queue control blocks, the UART RX DMA buffer at `0x2407CA80` (128 B). About 13 KB free — tight.

**D2 SRAM1 (`0x30000000`, 128 KB)** — `s_DfsdmBuf` at `0x30004000` (512 B). The other ~127 KB is free and available for future audio extensions or relocated heaps.

**D2 SRAM2 (`0x30020000`, 128 KB)** — `g_AudioBuf` at `0x30020000` (96 KB, 48000 × int16 PCM). The remaining ~32 KB is free.

**D3 SRAM4 (`0x38000000`, 64 KB)** — `s_upBuf` for SEGGER RTT (16 KB), `s_downBuf` (16 B), `_SEGGER_RTT` control block at `0x38000010` (96 B). The rest is unused.

**External SDRAM (`0xD0000000`, 32 MB)** — JPEG decode intermediates (`s_ycbcrBuf`, `s_rgb888Buf`, `s_rgb888Scaled`) totalling ~5.4 MB. Plenty of headroom.

---

## 6. Collision Audit

Placement audits are necessary but not sufficient. Once every buffer is mapped to a domain, you have to check that **the right master can reach each one** and that **adjacent buffers can't trample each other**. A few specific checks I run after every significant build:

| Check | Buffers | Domain | Status |
|---|---|---|---|
| Audio buffers in their own domain | `s_DfsdmBuf` (D2 SRAM1) + `g_AudioBuf` (D2 SRAM2) | D2 | ✓ DMA1 writes SRAM1, CPU writes SRAM2; 124 KB gap |
| UART RX pipeline same domain | `s_dma_rx_buf`, `s_rawBleQueueCB`, `s_rawBleQueueStorage`, `s_bleHistMutexCB` | D1 AXI | ✓ no cross-domain traffic per message |
| Framebuffer isolated from audio | `TouchGFX_Framebuffer` (D1 AXI) vs audio (D2) | mixed | ✓ different domains — eliminates FUIF |
| Heap vs DMA buffer | `ucHeap` (D1 AXI) vs `s_DfsdmBuf` (D2 SRAM1) | mixed | ✓ isolated by domain |
| Stack vs DMA buffers | `CSTACK` (DTCM) | D1 DTCM | ✓ DMA cannot reach DTCM — safe by design |
| Free space margins | D1 AXI 97.5% used, D2 SRAM1 99% free, D3 100% unused | — | ⚠ AXI is tight; move next big buffer to D2 |

A previous configuration on this project had the FreeRTOS heap adjacent to the DFSDM buffer in the same MPU region. Task-creation logic was stalling the audio pipeline because of competing bus traffic, and the bug was non-deterministic — sometimes audio came through, sometimes the pipeline stalled until the next reset. Moving the heap to AXI SRAM in D1 eliminated the contention completely. That issue is now resolved, but it's a textbook example of why "default placement" should never be trusted for DMA-adjacent buffers.

### Collision types and how we solved them

Three classes of collision can occur on an STM32H7 system, each with its own signature and its own fix. **(1) Spatial overlap** — two variables claim overlapping address ranges because the `.icf` doesn't carve out their regions correctly. This is the loudest failure mode: the linker errors out with `Lp011: section placement failed`, the build refuses to complete, and you have an explicit shortfall in bytes to triage. Fix: rebalance the section directives in the `.icf`, or move a large buffer to a different domain via `#pragma location`. **(2) Bus contention** — two masters try to drive the same bus segment at the same time. Symptoms are non-deterministic: jitter in audio capture, LCD FUIF interrupts during heavy CPU work, missing UART bytes during long SDMMC writes. The fix is to move one of the contenders to a different domain. On this project, the LCD framebuffer in D1 AXI and the DFSDM ring in D2 SRAM1 share no buses on the critical path, so they can run concurrently at full rate. **(3) Cache coherency collision** — the CPU cache and a DMA controller hold conflicting views of the same memory location. This is described in detail in the cache section below. Symptoms look like random data corruption — the kind that survives across resets but disappears when you add a `printf` because the print changes the cache eviction pattern. The fix is either an MPU Non-Cacheable region for the buffer, or explicit `SCB_CleanDCache_by_Addr` / `SCB_InvalidateDCache_by_Addr` calls bracketing every DMA transaction. Recognising which class you're facing is most of the diagnostic work: build-time errors are Type 1, performance-related symptoms are Type 2, "impossible" data values are usually Type 3.

The general principle: each domain should serve one and only one traffic class. Don't mix CPU-intensive RTOS objects with peripheral DMA buffers in the same region. If you find yourself doing that, either move one of them or accept the contention will eventually bite you.

### When to use D2 SRAM vs D1 AXI SRAM

There's a recurring question that comes up on every new buffer: should this live in AXI or in D2 SRAM? Both are reachable from the CPU. Both are reachable from DMA1/2. The decision criteria are: **(a) Who is the primary master?** AXI is faster from the CPU side (fewer wait states, on the M7's main bus), so put CPU-dominated data there. D2 is naturally adjacent to DMA1/2, so put peripheral-dominated data there. **(b) How big is the buffer?** Large buffers go to D2 because AXI is more contested (framebuffer, JPEG state, heap, queues, HAL handles all want AXI). A 96 KB audio accumulator in AXI would crowd everything else; the same buffer in D2 SRAM2 leaves AXI free. **(c) Does it cross the bus matrix on every access?** A buffer that the CPU touches once per frame and DMA touches 1000 times per second should be in the DMA's domain (D2). A buffer the CPU touches 10000 times per second and DMA touches once should be in AXI. On this project the rule played out cleanly: framebuffer → AXI (LTDC scans it as a D1 master), heap → AXI (FreeRTOS API touches it on every task switch), DFSDM ring → D2 SRAM1 (DMA writes it continuously), PCM accumulator → D2 SRAM2 (CPU writes it once per DMA chunk, SDMMC reads it once per file). When in doubt, the master that wins arbitration most often dictates the region.

---

## 7. Hard Rules — Where Each Buffer Class Belongs

| Buffer class | Required region | Why |
|---|---|---|
| LTDC framebuffer | D1 AXI SRAM (`0x24000000`) | LTDC scans on AHB; AXI removes contention with FMC/SDRAM |
| DFSDM DMA ring | D2 SRAM1 (`0x30000000`+) | DMA1 lives in D2; placing in D2 SRAM avoids AXI contention with LTDC |
| PCM accumulator (`g_AudioBuf`) | D2 SRAM2 (`0x30020000`+) | CPU-only writes, large (96 KB) — keeps D1 AXI free for graphics |
| SDMMC IDMA bounce | D1 AXI SRAM | SDMMC1 IDMA prefers AXI; bounce buffer copied from D2 is the standard pattern |
| UART RX DMA buffer | D1 AXI SRAM | DMA1/2 reachable; same domain as ISR-driven queue means no cross-domain hop per message |
| FreeRTOS queues / mutexes / TCBs | D1 AXI SRAM (default) | CPU-only access; keep with task stacks. Static allocation. |
| FreeRTOS heap (`ucHeap`) | D1 AXI SRAM | Default placement is correct. Never place adjacent to DMA ring buffers. |
| `CSTACK` + ISR stack | DTCM (`0x20000000`) | Zero wait, no DMA can corrupt it |
| JPEG decode intermediates | External SDRAM | Multi-megabyte, latency-tolerant |
| TouchGFX assets | QSPI Flash (XIP) / SDRAM | Read-only LUTs at runtime |
| SAI4 BDMA buffers (if ever used) | D3 SRAM4 (`0x38000000`) | BDMA is D3-only; will fail silently anywhere else |

The pattern: **place each buffer in the same domain as the master that drives it most frequently**. Cross-domain access works through the bus matrix, but it costs cycles and creates contention. Putting things in their natural domain isn't optimisation — it's the path of least surprise.

---

## 8. Managing the L1 Cache on STM32H7

Managing the L1 cache on the STM32H7 (Cortex-M7) is a mandatory requirement for system reliability. The core problem is a synchronisation gap between what the CPU sees and what the hardware actually does. To achieve high performance, the CPU reads and writes data to a local, high-speed cache rather than slower physical RAM. However, peripheral DMA controllers — such as those for UART, SAI, or Ethernet — bypass this cache entirely and talk directly to the RAM.

### Write and read coherency failures

This mismatch creates two distinct failure modes. **First**, if the CPU writes data to a buffer, the bytes might only sit in the cache, leaving the DMA to transmit *old* data from the RAM — a "Write-Dirty" issue. **Second**, the DMA might update the RAM while the CPU continues to read *stale* values from its cache — a "Read-Stale" issue.

To address this correctly, you must either use the MPU to define specific DMA regions as Non-Cacheable, or manually perform Cache Maintenance operations: **Clean** (forcing cache data out to RAM before a DMA transmit) and **Invalidate** (clearing the cache so the CPU is forced to read fresh data from RAM after a DMA receive). Failure to do so results in random, non-deterministic data corruption that can silently break communication protocols and audio streams. Worst of all, these bugs appear as random software glitches rather than the hardware configuration errors they truly are.

In practice on this project, the audio buffers (`s_DfsdmBuf` and `g_AudioBuf`) live in MPU-configured Non-Cacheable Shareable regions of D2 SRAM. That means the M7 reads and writes them straight through to RAM, no cache involvement, no invalidate/clean dance required. The performance hit is small (audio is slow data), and the simplification is large. For the UART RX buffer in AXI, where caching is on, the ISR does `SCB_InvalidateDCache_by_Addr(s_dma_rx_buf, BLE_DMA_BUF_SIZE)` immediately before reading the bytes DMA just wrote. The 32-byte alignment requirement is non-negotiable; pad your buffer declarations with `__attribute__((aligned(32)))` and size them in 32-byte multiples.

### Cache considerations across the project

There are three patterns that recur for cache management on this architecture and it's worth being explicit about each. **Pattern A — Non-Cacheable MPU region for DMA buffers.** This is the simplest and most foolproof: declare an MPU region covering the buffer's address range with the Non-Cacheable and Shareable attributes, and the CPU bypasses the cache for every access to that region. No `Clean`, no `Invalidate`, no alignment headaches. The cost is slightly slower CPU access (because every read/write hits RAM directly), but for audio buffers updated at 16 kHz this is invisible. We use this for `s_DfsdmBuf` and `g_AudioBuf` in D2 SRAM. **Pattern B — Cacheable region with explicit maintenance.** For high-throughput DMA paths where bypassing the cache would actually hurt (e.g., a buffer the CPU also processes heavily), keep it cacheable but bracket every DMA transaction with `SCB_CleanDCache_by_Addr` before a transmit and `SCB_InvalidateDCache_by_Addr` before reading after a receive. We use this for the UART RX path: the buffer is in AXI, caching is on, the ISR invalidates the cache lines immediately before reading the DMA-deposited bytes. The discipline must be applied at every call site; missing a single invalidate produces intermittent corruption. **Pattern C — Cache-coherent regions (TCM and the AXI-cache mode where applicable).** DTCM and ITCM are by construction outside the cache hierarchy — no `Clean`, no `Invalidate`, no MPU configuration needed. This is why the M7 stack lives in DTCM: cache coherency is just not a concept that applies there. For framebuffers in AXI, we rely on the LTDC controller's burst-read semantics combined with the CPU's write-back cache being flushed on each DMA2D blit boundary; TouchGFX's framework handles the discipline transparently. The general decision tree: small DMA buffers → Pattern A; high-throughput CPU+DMA shared buffers → Pattern B; CPU-private fast data → DTCM / Pattern C. Pick one pattern per buffer, document the choice in a comment next to the declaration, and never mix patterns within a single buffer.

---

## 9. Section Attribution Cheat Sheet

```c
/* Default .bss/.data — goes to D1 AXI SRAM by linker default */
static uint32_t g_counter;

/* Explicit AXI placement (rare, but documents intent) */
#pragma location = ".axi_sram"
static StaticQueue_t s_queueCB;

/* D2 SRAM1 — DMA1/DMA2 reachable */
#pragma location = ".sram1"
static uint8_t s_dmaBuf[256];

/* D2 SRAM2 — DMA1/DMA2 reachable */
#pragma location = ".sram2"
static int16_t s_audioBlock[8192];

/* Absolute address — for hardware-fixed buffers */
#pragma location = 0x30004000
static __no_init uint32_t s_DfsdmBuf[128];
```

For the named-section approach to work, the `.icf` must define each section's region:

```
define region SRAM1_region = mem:[from 0x30000000 to 0x3001FFFF];
place in SRAM1_region { section .sram1 };
```

Without that, the named section silently falls back to default `.bss` placement, which is usually D1 AXI. I had a buffer attributed to a section name `.dma_buf` that "worked" for months because AXI happened to be DMA-reachable — but it was placed by accident, not by design. A later refactor moved adjacent symbols, the silently-misplaced buffer shifted with them, and a regression appeared. Section attribution is only protection if the `.icf` enforces it.

---

## 10. Debugging a Real-Time System — cspybat, C-SPY macros, and SEGGER RTT

The hardest part of debugging real-time embedded firmware isn't finding bugs in the code — it's finding them without altering the timing of the system enough to make the bug disappear. At 400-480 MHz on a Cortex-M7, a single `printf` to a UART can stall the calling task for several milliseconds while the bytes shift out the wire. That's enough to mask a race condition, hide a priority-inversion deadlock, or smear an audio glitch across so many frames you can't localise it. The whole point of using purpose-built embedded-debug tooling is to inspect the system *without* changing its behaviour. Three tools in the IAR + SEGGER ecosystem make this practical on STM32: `cspybat`, C-SPY macros, and SEGGER RTT. They complement each other; mature projects use all three.

### SEGGER RTT — zero-overhead real-time logging

RTT (Real-Time Transfer) replaces `printf` entirely on this project. Instead of writing bytes to a UART peripheral, the firmware writes them to a ring buffer in target RAM. The J-Link probe, attached via SWD, reads the ring buffer over the DAP (Debug Access Port) at roughly 1 ms intervals and streams the bytes to a host-side viewer (`JLinkRTTViewer.exe`) or logger (`JLinkRTTLogger.exe`). The cost on the target side is one cache-line write per message and an atomic increment of the write index — a handful of CPU cycles, no peripheral involvement, no interrupts, no scheduling impact. We use this for every `RLOG()` call in the firmware. The boot log, per-PING health counters, audio quality reports, DFSDM self-checks, and STREAM2/3 handshake markers all flow through RTT channel 0. On this project the RTT control block and ring buffer sit in D3 SRAM4 (`0x38000000`) — completely outside the main AXI memory where the application's data lives. J-Link's RAM-scan finds the `"SEGGER RTT"` magic signature there at session start, and from then on the RTT channel is "free" in the sense that turning it off would not give us a faster boot or a roomier AXI. For long captures, `JLinkRTTLogger.exe -rttsearchranges "0x38000000 0x10000" -rttchannel 0 log.txt` writes the channel directly to a file without holding a debug session open.

### `cspybat` — command-line C-SPY for automated probes

`cspybat` is IAR's headless invocation of the C-SPY debug engine. It loads a `.out` file, attaches the configured probe (J-Link, in our case), runs a macro file, and exits — all from a shell prompt or batch script. The killer use case is **automated post-build verification**: every time the firmware is built, a CI step (or a developer's `.bat` file) can run a probe that flashes the binary, lets it boot, halts at a known function, dumps the relevant peripheral registers and global variables, and writes a log file the developer reads. No human in the loop. On this project we have probe scripts for boot-stage verification (25 checkpoints across `MX_*_Init`), PFB strip dispatch verification, LTDC/DSI pipeline state, and DFSDM register snapshot capture at button-press EXTI. Each one is a one-line invocation of `cspybat.exe ... --macro=probe.mac > log.txt`. The boot probe alone has caught a dozen regressions over the project's lifetime — clock-tree misconfigurations, MPU regions that drifted, peripheral init order that broke after a CubeMX regenerate — all without anyone opening the IAR IDE.

### C-SPY macros — programmable debugger logic

The C-SPY macro language is the scripting layer that sits inside `cspybat` (or the interactive IDE). A `.mac` file can install breakpoints by symbol name, read C variables and struct fields directly (the debugger resolves symbols from the `.out`/ELF, you don't grep the `.map`), write to peripheral registers, advance the CPU between checkpoints, and emit formatted text to the log. The canonical pattern is `__setCodeBreak("MX_LTDC_Init", 0, "1", "TRUE", "onHit()")`, where `onHit()` is a macro function that dumps whatever state matters at that breakpoint and the session continues. For multi-checkpoint flows we use `__hwRunToBreakpoint(&main, 5000)` to chain advances. The macro layer is what makes C-SPY genuinely powerful versus a generic GDB session — symbol-aware breakpoints survive a rebuild (no address re-extraction), struct-field reads work without manual offset arithmetic, and the same `.mac` file can be replayed in the IDE for interactive use. There is one specific quirk to be aware of on this project: with passive `__setCodeBreak`, only the first breakpoint hit reliably runs its action under `cspybat`; for multi-BP flows we use an orchestrator (PowerShell) that runs `cspybat` once per breakpoint, substituting the target symbol via a template `.mac.tpl` file. The active `__hwRunToBreakpoint` flow doesn't have this limit.

### Choosing the right tool for the question

The three tools answer different questions and don't substitute for each other. **RTT** answers "what is the firmware doing right now?" — continuous narrative trace. **`cspybat` + C-SPY macros** answer "what is the state at this specific symbol when execution reaches it?" — symbol-resolved snapshots. **J-Link Commander** (a fourth tool we use but didn't expand on here) answers "what is in this memory address while the target keeps running?" — free-running memory reads with no halt. Together they replace `printf` debugging entirely. On a 400 MHz Cortex-M7 running a partial-framebuffer LCD pipeline alongside live audio capture, `printf` was never going to be the right answer. RTT plus C-SPY plus J-Link memory probing is.

---

## 11. References

- **AN4861** — LTDC peripheral, FUIF/FIFO underrun causes and bandwidth math
- **AN4891** — STM32H7 system architecture overview. Bus matrix, domains, DMA reach
- **AN5215** — Memory bandwidth and performance numbers
- **AN5405** — Cache and coherency on Cortex-M7. Required reading
- **RM0399** — Reference manual. Chapter 2 (memory map), chapter 8 (RCC), bus-matrix appendix
- **UM2411** — SAI peripheral, PDM input mode
- **SEGGER RTT documentation** — wire protocol, control-block layout, host tools
- **SEGGER J-Link/J-Trace User Guide (UM08001)** — J-Link Commander commands, SWO, RTT
- **IAR C-SPY Debugging Guide (UCSARM-26)** — macro language reference, cspybat invocation
- **IAR Linker and Library Tools Reference Guide** — `.icf` syntax, section placement, `#pragma location`

For the IAR-specific compiler syntax (`#pragma location`, `@` operator, `.icf` placement directives), the **IAR C/C++ Development Guide** ships with EWARM.

---

## 12. Conclusion

Five rules cover most of the placement decisions on STM32H7:

1. **Pick the domain that owns the master that uses the buffer.** DFSDM in D2 means audio buffers in D2 SRAM. LTDC in D1 means framebuffer in D1 AXI.
2. **Enforce strict static allocation.** Heap-allocated objects move when the heap moves; static objects have fixed addresses you can audit in the `.map` and inspect via C-SPY without instrumentation.
3. **Use mandatory IAR placement — `#pragma location`, the `@` operator, or section attributes** — for every critical variable. Never let a critical buffer float in default `.bss` or `.data`.
4. **Minimise cross-domain bus traffic.** Each domain serves one traffic class. Heap and DMA buffers don't share a region. UART DMA buffer and its consumer queue stay in the same domain.
5. **Adopt zero-footprint debugging — ITM, SWO, and `cspybat` C-SPY macros** — instead of `printf`. At 480 MHz, `printf` alters the system under test; ITM doesn't.

Two cautions sit alongside the rules. Always define explicit MPU regions for every memory area accessed by DMA, marking them Bufferable / Non-Cacheable; without that, the CPU cache and the DMA controller will hold conflicting versions of the same data, and you'll spend days hunting non-deterministic corruption. And always use Mutexes (not binary semaphores) for shared resources in a multitasking system, because Mutexes support priority inheritance and prevent lower-priority tasks from inadvertently blocking higher-priority ones.

The practical takeaway: memory placement on H7 is part of the design, not part of the optimisation pass. Treat each new buffer like you treat each new task — give it a name, a domain, a reason, and a verified location in the `.map` — and the worst class of "works on Tuesday, breaks on Wednesday" bugs goes away.

---

*Built and debugged with IAR Embedded Workbench 9.70.2 (`cspybat` 9.4.6.1706), SEGGER J-Link V9.30, on the STM32H747I-DISCO board.*
