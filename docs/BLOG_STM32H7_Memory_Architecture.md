# STM32H7 Memory Architecture in Practice — A Real-World Guide for TouchGFX, FreeRTOS and DMA-Heavy Embedded Systems

*Engineering notes from a voice-controlled music player built on the STM32H747I-DISCO, where memory placement decisions were the difference between a working system and a frozen LCD.*

*For embedded software and hardware engineers working with STMicroelectronics MCUs — particularly anyone moving from an STM32F4 / L4 to the H7 family for the first time, or anyone debugging unexplained DMA, cache, or LCD-tearing bugs on a dual-core H7.*

---

## 1. Introduction — Why STM32H7 Memory Architecture Matters

### TL;DR

The STM32H747 is a high-end dual-core Cortex-M7 + Cortex-M4 MCU with **three independent power domains, eight SRAM regions, four DMA controllers, and a multi-master bus matrix**. Default linker placement works for trivial projects and quietly breaks for everything more complex — audio capture stutters, LCD framebuffers tear, UART RX silently drops bytes. This post documents the architectural rules that prevent those failure modes, with worked examples from a real TouchGFX + FreeRTOS + audio-streaming project. If you read only one section, read [Section 8: Hard Rules — Where Each Buffer Class Belongs](#8-hard-rules-where-each-buffer-class-belongs-on-stm32h7).

### Motivation — the problem this post solves

If you've ever built anything beyond a hello-blink on an STM32H7, you've probably hit at least one of these:

- Audio capture works on the bench but glitches every few seconds when the LCD redraws.
- A FreeRTOS queue that worked yesterday breaks today after an unrelated refactor — and no code in your queue path changed.
- LCD tearing or FUIF (FIFO underrun) interrupts firing during heavy CPU work, with no obvious culprit.
- DMA transfers that "complete successfully" but the CPU reads stale data, or transmits stale data.
- Random HardFaults that disappear when you add a `printf` for debugging.

Every one of these is a **memory architecture symptom**. None of them are bugs you can find by reading the code line by line. They're bugs that come from the H7's distributed memory map being treated as a flat SRAM blob, which is the model embedded engineers carry over from simpler MCUs. The H7 doesn't work that way, and the silicon won't tell you when you've placed a buffer in a region that doesn't suit it — the compiler is silent, the linker is silent, and at runtime the bug shows up as non-deterministic glitches.

This post is a practical walkthrough of how to think about H7 memory placement *before* those symptoms appear, written from the trenches of a real product. Everything here was learned the hard way; everything is traceable to a `.map` file, a register dump, or an RTT log.

### A high-level overview of the STM32H747

The STM32H747 is at the top end of STMicroelectronics's general-purpose Cortex-M lineup. The key specs that drive everything in this post:

- **Dual-core architecture.** A Cortex-M7 main core running up to 480 MHz (400 MHz in our build for thermal headroom), plus a Cortex-M4 co-processor running up to 240 MHz. The two cores share Flash and most peripherals but each has its own L1 cache, its own NVIC, and its own bus-master ports into the system bus matrix. On this project we use the M7 exclusively; the M4 holds a stop-mode stub for future low-power offload.
- **Three independent power domains (D1, D2, D3).** Each domain has its own clock gate, voltage regulator, and SRAM. Domains can be powered down independently, which enables low-power modes where only D3 stays alive listening for wake events.
- **~1 MB of internal SRAM total, split across at least five distinct regions** — D1 AXI SRAM (512 KB), D2 SRAM1/2/3 (288 KB combined), D3 SRAM4 (64 KB), plus D1 DTCM (128 KB) and ITCM (64 KB) tightly coupled to the M7.
- **2 MB Flash, dual-bank, with read-while-write support** for in-field firmware updates.
- **Four DMA controllers** with different domain reach: MDMA (cross-domain), DMA1/DMA2 (D2-rooted, can reach D1+D2), BDMA (D3-only). Plus DMA2D for graphics blitting and dedicated IDMAs inside SDMMC and JPEG.
- **A rich peripheral set:** dual Ethernet MAC, USB OTG HS/FS, multiple SAI (audio), DFSDM (digital filter for sigma-delta — what we use for PDM mic capture), FMC (external memory controller), QUADSPI, LTDC (LCD-TFT controller) with up to 24-bit RGB output, DSI host for MIPI-DSI panels, hardware JPEG codec, DMA2D 2D blitter, two SDMMC controllers, and the usual array of UARTs, SPIs, I2Cs, timers and ADCs.
- **High-end Cortex-M7 features:** 16 KB + 16 KB L1 caches (instruction + data), single-precision + double-precision FPU, MPU with 16 regions, and full CoreSight trace (ETM, ITM, DWT) brought out to SWO and trace pins.

### What ST built this for

The H7 is targeted at applications that need MCU-level real-time determinism *and* near-MPU-level compute. The canonical use cases are: graphical user interfaces driving large colour TFT panels (TouchGFX, embedded Wizard, LVGL); industrial control with simultaneous Ethernet + CAN + motor-control PWM; high-resolution audio capture, processing and streaming (PDM mic arrays, multi-channel SAI, USB Audio Class); medical and instrumentation devices that combine signal acquisition, on-device DSP, and a touch screen; and any product where a single chip needs to drive a display, talk to a cloud, and run sensor pipelines simultaneously. This project — voice capture, cloud STT, and a TouchGFX UI on one chip — is squarely in that envelope.

### The specific project this post is built from

This post is a retrospective on the memory-architecture work for a voice-controlled music player built on the **STM32H747I-DISCO** development board. The STM32 records audio from the onboard MP34DT05-A PDM microphone using DFSDM in Sinc3 mode at 16 kHz, persists the WAV file to SD card via SDMMC, and streams it over UART8 to a Wi-Fi companion module that handles the cloud upload and speech-to-text call. The companion module is treated as an opaque endpoint in this post — every technical detail below concerns the STM32 side: how the audio buffers are placed, how the DMA paths are routed, how the FreeRTOS objects are pinned, and how the LCD framebuffer coexists with the audio path on the bus matrix. In parallel with audio, a TouchGFX UI renders to the 800×480 RGB DSI panel using a partial-framebuffer (PFB) strategy. FreeRTOS coordinates seven concurrent tasks on the M7: voice recording, SD-card writing, UART RX, command handling, RTT logging, TouchGFX rendering, and a low-priority video-frame task.

The toolchain used throughout this post: **IAR Embedded Workbench 9.70.2** for compile + link, **`cspybat`** (IAR's command-line C-SPY runner) for headless automated debug probes, **C-SPY macros** (`.mac` files) for scripted breakpoint actions, **SEGGER J-Link** as the SWD probe, **SEGGER RTT** for zero-overhead real-time logging, **SEGGER J-Link RTT Viewer** (the standalone GUI host tool) for watching RTT channels live during development, **JLinkRTTLogger** for capturing RTT channels to file unattended, and **J-Link Commander** for free-running register/memory inspection while the target runs. The combination of these tools is what made it feasible to diagnose the memory-related bugs described later in the post without ever using `printf`.

### What you'll learn from this post

The H7's three-domain layout and the engineering reasons ST chose it. The fixed peripheral memory map and which DMA controllers can reach which peripherals. How the AHB bus matrix and MDMA tie the domains together. A concrete block diagram of one real project's tasks and buffers. How to read a linker `.map` file to verify placement decisions. The three classes of memory collision (spatial, bus contention, cache coherency) and how to fix each. A practical workflow for debugging real-time systems with zero CPU overhead using SEGGER RTT, `cspybat`, and C-SPY macros. A short rule book I now apply to every H7 project.

Every example below is from real firmware on this project and every decision is traceable back to a `.map` file or a `.icf` linker script committed to the project repository.

---

## 2. STM32H7 Memory Domain Overview

![STM32H7 memory architecture diagram showing the three power domains D1 D2 D3 with their SRAMs and DMA controllers](images/stm32h7-memory-architecture-domains.png "STM32H7 memory architecture — three-domain layout (D1 core, D2 peripheral, D3 low-power)")

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

## 3. STM32H747 Peripheral Memory Map — Where Each Peripheral Lives

Beyond the SRAM regions, every peripheral on the H747 lives at a **fixed memory address**. These addresses are baked into the silicon and never change; they're how the CPU and DMA controllers reach each peripheral's registers. Knowing this map matters for three concrete reasons: (a) choosing the right DMA controller, because each peripheral has fixed DMA request lines that pair with specific DMA streams; (b) debugging via direct memory access, because a J-Link command like `mem32 0x40017000` reads DFSDM1 channel 0's `CHCFGR1` register regardless of whether any debug symbols are loaded; and (c) understanding why peripherals in the same domain share clock-gate behaviour and power-mode reach.

The table below summarises the major peripheral bus segments on the STM32H747. Each row maps an address range to its bus (APB1–4 or AHB1–4), its power domain (D1, D2, or D3), and the key peripherals living there. Numbered descriptions in sections 3.1–3.8 below explain each row in detail.

| #   | Address range              | Bus     | Domain | Key peripherals living in this segment                                     |
|-----|---------------------------|---------|--------|----------------------------------------------------------------------------|
| 3.1 | `0x4000_0000 - 0x4000_FFFF` | APB1    | D2     | TIM2-7, TIM12-14, LPTIM1, SPI2/3, SPDIFRX, USART2/3, **UART4-8**, I2C1-3, DAC1, FDCAN |
| 3.2 | `0x4001_0000 - 0x4001_FFFF` | APB2    | D2     | TIM1, TIM8, TIM15-17, USART1/6, SPI1/4/5, SAI1/2/3, **DFSDM1**, HRTIM      |
| 3.3 | `0x4002_0000 - 0x4007_FFFF` | AHB1    | D2     | **DMA1, DMA2, DMAMUX1**, ADC1/2, Ethernet MAC, USB1 OTG_HS                |
| 3.4 | `0x4800_0000 - 0x4802_FFFF` | AHB2    | D2     | DCMI, CRYP, HASH, RNG, SDMMC2                                              |
| 3.5 | `0x5000_0000 - 0x53FF_FFFF` | AHB3    | D1     | **LTDC, MDMA, DMA2D, JPEG**, FMC, QUADSPI, SDMMC1, FLASH interface         |
| 3.6 | `0x5800_0000 - 0x5800_3FFF` | APB4    | D3     | SYSCFG, LPUART1, SPI6, I2C4, LPTIM2/3/4/5, COMP1/2, VREFBUF, **RTC, SAI4** |
| 3.7 | `0x5802_0000 - 0x5806_FFFF` | AHB4    | D3     | **GPIOA-K**, CRC, **BDMA**, ADC3, HSEM, **RCC, PWR**                       |
| 3.8 | `0xE000_0000 - 0xE00F_FFFF` | Private | M7 core | NVIC, SCB, SysTick, MPU, DWT, ITM, FPB, ETM, DAP                          |

### 3.1 APB1 — D2 peripheral domain, slower bus

APB1 is the lower-speed peripheral bus, running at HCLK/2 (typically 100 MHz when the M7 is at 400 MHz). It hosts the bulk of general-purpose communication peripherals: the lower-numbered timers (TIM2-7 and TIM12-14), most UARTs (UART4-8, USART2/3), all I2C controllers, most SPI controllers, the DAC, FDCAN, and SPDIFRX. **UART8** on this project — the link to the Wi-Fi companion module — is mapped at `0x4000_7C00`. APB1's relatively low clock means most APB1 peripherals can be driven at line rate without contention even when D1 is heavily loaded.

### 3.2 APB2 — D2 peripheral domain, faster bus

APB2 also runs at HCLK/2 (100 MHz max in the typical configuration), but its peripherals are the higher-bandwidth ones: the advanced timers (TIM1, TIM8, TIM15-17, HRTIM), the fast SPI/SAI controllers, USART1/6, and crucially **DFSDM1**, which drives the PDM microphone capture on this project. DFSDM1 sits at `0x4001_7000`. Its DMA pairings route to DMA1/DMA2 channels through DMAMUX1; both of those DMA controllers live in the same D2 domain, which is why DFSDM-to-RAM transfers don't need to cross the bus matrix to reach D2 SRAM1.

### 3.3 AHB1 — D2 peripheral domain, DMA + high-speed peripherals

AHB1 is the high-speed peripheral bus, running at HCLK directly (200 MHz). It hosts the **DMA controllers (DMA1, DMA2)** and their request multiplexer (DMAMUX1), along with the Ethernet MAC, USB1 OTG_HS, and the first two ADCs. The DMA controllers' register banks sit at `0x4002_0000` (DMA1) and `0x4002_0400` (DMA2). When firmware writes to a DMA stream's configuration registers (CR, NDTR, M0AR, etc.), it's writing here — the actual data movement happens through the AHB-master ports of the DMA blocks, not through these register accesses.

### 3.4 AHB2 — D2 peripheral domain, security + camera + SD

AHB2 hosts the security/crypto block (CRYP, HASH, RNG), the digital camera interface (DCMI), and the second SD/MMC controller (SDMMC2). SDMMC2 is functionally identical to SDMMC1 (which is on AHB3 in D1) — the difference is which domain's DMA can reach it most cheaply. SDMMC2 in D2 pairs naturally with DMA1/2 in D2; SDMMC1 in D1 pairs naturally with MDMA.

### 3.5 AHB3 — D1 core domain, graphics + JPEG + memory controllers

AHB3 is the D1 master bus and is where the high-bandwidth memory controllers live: **FMC** (for external SDRAM, SRAM, NOR or NAND), **QUADSPI** (for external Flash), and the first SD/MMC controller (SDMMC1). AHB3 also hosts the bulk-DMA and graphics engines the M7 needs to drive the LCD pipeline: **MDMA** at `0x5200_0000` (the cross-domain master-mode DMA), **DMA2D** (the 2D blitter), the **JPEG codec**, and **LTDC** (the display controller). On this project all of them are in use: JPEG decodes the cover-art thumbnails, DMA2D blits the decoded RGB into the framebuffer, and LTDC scans the framebuffer to the DSI panel at 60 Hz.

### 3.6 APB4 — D3 low-power domain

APB4 is the low-power peripheral bus. Anything that needs to stay alive in Stop or Standby modes lives here: the **RTC** for time-of-day wakeups, **SAI4** for low-power audio listening (when used), **LPUART1** for low-power serial wake patterns, **LPTIM2-5** for low-power timing, and the low-speed comparators COMP1/2. All of these can run on the LSE (32.768 kHz) or LSI (~32 kHz) clock sources while the main HSE/PLL clocks are gated off — which is exactly what enables the "deep sleep, wake on event" power profile that makes the H7 suitable for battery-powered designs.

### 3.7 AHB4 — D3 low-power domain, GPIO + clock + power

AHB4 hosts the foundational always-on peripherals: every GPIO bank (**GPIOA-K** starting at `0x5802_0000`), the **RCC** (Reset and Clock Control) block, the **PWR** (Power Control) block, the **BDMA** (the D3-local DMA that can only reach SRAM4), ADC3, the hardware semaphore **HSEM** for dual-core synchronisation, and the CRC engine. Notably, **all GPIO control registers live in D3**. Even when you're toggling a GPIO from a D1 task running at 400 MHz, the write crosses the bus matrix into D3. For most cases this is invisible because the GPIO operation is single-cycle from the CPU's perspective, but it's worth knowing when chasing the last cycle of bit-bang timing or when a low-power mode unexpectedly stops a GPIO from responding.

### 3.8 Cortex-M7 Private Peripheral Bus (PPB)

The PPB isn't a chip-level bus — it's a Cortex-M7 architectural feature. The Private Peripheral Bus hosts the core's own debug and control blocks: **NVIC** (interrupt controller), **SCB** (system control block including the cache maintenance registers), SysTick, **MPU** (memory protection unit), **DWT** (data watchpoint and trace), **ITM** (instrumentation trace), FPB (flash patch and breakpoint), ETM (embedded trace macrocell), and the DAP that the J-Link probe talks to. These registers are CPU-private — they're not on any AHB or APB segment, and DMA cannot reach them. The cache-maintenance functions `SCB_CleanDCache_by_Addr` and `SCB_InvalidateDCache_by_Addr` are register writes to addresses in this region.

---

## 4. STM32H7 Bus Matrix and Domain Placement

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

## 5. Software Component Block Diagram — Audio + UART + LCD Pipeline

Here's how the moving parts of this specific project relate on the STM32 side. The audio path lives entirely in D2. The UART RX queue is in D1 AXI. RTT diagnostic buffers live in D3 SRAM4. LCD framebuffer is in D1 AXI. Tasks have priorities that let `VoiceRecTask` (32) preempt almost everything, with `UARTReceiveTask` (26) close behind, and `TouchGFXTask` (High, ~24) rendering the UI in the background. The right-hand "Wi-Fi companion" branch in the diagram is intentionally simplified — what matters for this post is the STM32-side UART RX / TX path and where its buffers live.

```
+---------------------+               +---------------------+
|  MP34DT05-A PDM mic |               |  Wi-Fi companion    |
|  CLK on PC2 (2MHz)  |               |  module (opaque)    |
|  Data on PC1        |               |  UART8 @ 921600     |
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

## 6. Memory Placement Audit — Reading the IAR `.map` File

Understanding the STM32H747 memory architecture is essential before assigning any buffer or RTOS object to a region. The `.map` file in IAR is the final, comprehensive report generated by the linker after each build, and it is the **source of truth** for the physical location of every function and variable in the MCU's memory. While the `.icf` file acts as the blueprint or plan, the `.map` file shows the actual execution: it details exactly which address was assigned to every symbol, its size, and which memory section it belongs to.

For an STM32H7 developer, this is the most essential tool for verifying that placement was successful. It allows you to confirm that DMA buffers are sitting in SRAM1 (and not accidentally in DTCM, which is inaccessible to DMA), or to ensure that your static queues are properly located in the AXI SRAM as intended. Analysing the map file is crucial for identifying memory hogs, preventing overflow, and resolving complex bus contention issues by verifying a total physical separation between different system variables.

**Example `.map` entry from this project:**

```
s_DfsdmBuf              0x30004000      0x200  Data  Lc  voice_recorder.o [5]
                        ^address        ^size              ^object file
```

So `s_DfsdmBuf` is 512 bytes (`0x200`) at `0x30004000` — inside D2 SRAM1. That matches the `#pragma location = 0x30004000` directive declared in our source file.

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

## 7. Collision Audit — Bus, Cache, and Spatial Conflicts

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

## 8. Hard Rules — Where Each Buffer Class Belongs on STM32H7

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

## 9. Managing the Cortex-M7 L1 Cache on STM32H7

Managing the L1 cache on the STM32H7 (Cortex-M7) is a mandatory requirement for system reliability. The core problem is a synchronisation gap between what the CPU sees and what the hardware actually does. To achieve high performance, the CPU reads and writes data to a local, high-speed cache rather than slower physical RAM. However, peripheral DMA controllers — such as those for UART, SAI, or Ethernet — bypass this cache entirely and talk directly to the RAM.

### Write and read coherency failures

This mismatch creates two distinct failure modes. **First**, if the CPU writes data to a buffer, the bytes might only sit in the cache, leaving the DMA to transmit *old* data from the RAM — a "Write-Dirty" issue. **Second**, the DMA might update the RAM while the CPU continues to read *stale* values from its cache — a "Read-Stale" issue.

To address this correctly, you must either use the MPU to define specific DMA regions as Non-Cacheable, or manually perform Cache Maintenance operations: **Clean** (forcing cache data out to RAM before a DMA transmit) and **Invalidate** (clearing the cache so the CPU is forced to read fresh data from RAM after a DMA receive). Failure to do so results in random, non-deterministic data corruption that can silently break communication protocols and audio streams. Worst of all, these bugs appear as random software glitches rather than the hardware configuration errors they truly are.

In practice on this project, the audio buffers (`s_DfsdmBuf` and `g_AudioBuf`) live in MPU-configured Non-Cacheable Shareable regions of D2 SRAM. That means the M7 reads and writes them straight through to RAM, no cache involvement, no invalidate/clean dance required. The performance hit is small (audio is slow data), and the simplification is large. For the UART RX buffer in AXI, where caching is on, the ISR does `SCB_InvalidateDCache_by_Addr(s_dma_rx_buf, BLE_DMA_BUF_SIZE)` immediately before reading the bytes DMA just wrote. The 32-byte alignment requirement is non-negotiable; pad your buffer declarations with `__attribute__((aligned(32)))` and size them in 32-byte multiples.

### Cache considerations across the project

There are three patterns that recur for cache management on this architecture and it's worth being explicit about each. **Pattern A — Non-Cacheable MPU region for DMA buffers.** This is the simplest and most foolproof: declare an MPU region covering the buffer's address range with the Non-Cacheable and Shareable attributes, and the CPU bypasses the cache for every access to that region. No `Clean`, no `Invalidate`, no alignment headaches. The cost is slightly slower CPU access (because every read/write hits RAM directly), but for audio buffers updated at 16 kHz this is invisible. We use this for `s_DfsdmBuf` and `g_AudioBuf` in D2 SRAM. **Pattern B — Cacheable region with explicit maintenance.** For high-throughput DMA paths where bypassing the cache would actually hurt (e.g., a buffer the CPU also processes heavily), keep it cacheable but bracket every DMA transaction with `SCB_CleanDCache_by_Addr` before a transmit and `SCB_InvalidateDCache_by_Addr` before reading after a receive. We use this for the UART RX path: the buffer is in AXI, caching is on, the ISR invalidates the cache lines immediately before reading the DMA-deposited bytes. The discipline must be applied at every call site; missing a single invalidate produces intermittent corruption. **Pattern C — Cache-coherent regions (TCM and the AXI-cache mode where applicable).** DTCM and ITCM are by construction outside the cache hierarchy — no `Clean`, no `Invalidate`, no MPU configuration needed. This is why the M7 stack lives in DTCM: cache coherency is just not a concept that applies there. For framebuffers in AXI, we rely on the LTDC controller's burst-read semantics combined with the CPU's write-back cache being flushed on each DMA2D blit boundary; TouchGFX's framework handles the discipline transparently. The general decision tree: small DMA buffers → Pattern A; high-throughput CPU+DMA shared buffers → Pattern B; CPU-private fast data → DTCM / Pattern C. Pick one pattern per buffer, document the choice in a comment next to the declaration, and never mix patterns within a single buffer.

---

## 10. IAR Section Attribution Cheat Sheet — `#pragma location` Syntax

**Example — typical placements from this project (variable names are illustrative; pick your own naming for your own project):**

```c
/* Example: default .bss/.data — goes to D1 AXI SRAM by linker default */
static uint32_t g_counter;

/* Example: explicit AXI placement (rare, but documents intent) */
#pragma location = ".axi_sram"
static StaticQueue_t s_queueCB;

/* Example: D2 SRAM1 — DMA1/DMA2 reachable */
#pragma location = ".sram1"
static uint8_t s_dmaBuf[256];

/* Example: D2 SRAM2 — DMA1/DMA2 reachable */
#pragma location = ".sram2"
static int16_t s_audioBlock[8192];

/* Example: absolute address — for hardware-fixed buffers like the DFSDM ring */
#pragma location = 0x30004000
static __no_init uint32_t s_DfsdmBuf[128];
```

For the named-section approach to work, the `.icf` must define each section's region. **Example `.icf` snippet:**

```
define region SRAM1_region = mem:[from 0x30000000 to 0x3001FFFF];
place in SRAM1_region { section .sram1 };
```

Without that, the named section silently falls back to default `.bss` placement, which is usually D1 AXI. I had a buffer attributed to a section name `.dma_buf` that "worked" for months because AXI happened to be DMA-reachable — but it was placed by accident, not by design. A later refactor moved adjacent symbols, the silently-misplaced buffer shifted with them, and a regression appeared. Section attribution is only protection if the `.icf` enforces it.

---

## 11. Debugging a Real-Time STM32H7 System — cspybat, C-SPY macros, and SEGGER RTT

The hardest part of debugging real-time embedded firmware isn't finding bugs in the code — it's finding them without altering the timing of the system enough to make the bug disappear. At 400-480 MHz on a Cortex-M7, a single `printf` to a UART can stall the calling task for several milliseconds while the bytes shift out the wire. That's enough to mask a race condition, hide a priority-inversion deadlock, or smear an audio glitch across so many frames you can't localise it. The whole point of using purpose-built embedded-debug tooling is to inspect the system *without* changing its behaviour. Three tools in the IAR + SEGGER ecosystem make this practical on STM32: `cspybat`, C-SPY macros, and SEGGER RTT. They complement each other; mature projects use all three.

### SEGGER RTT — zero-overhead real-time logging

RTT (Real-Time Transfer) replaces `printf` entirely on this project. Instead of writing bytes to a UART peripheral, the firmware writes them to a ring buffer in target RAM. The J-Link probe, attached via SWD, reads the ring buffer over the DAP (Debug Access Port) at roughly 1 ms intervals and streams the bytes to a host-side tool. The cost on the target side is one cache-line write per message and an atomic increment of the write index — a handful of CPU cycles, no peripheral involvement, no interrupts, no scheduling impact. We use this for every `RLOG()` call in the firmware. The boot log, per-PING health counters, audio quality reports, DFSDM self-checks, and STREAM2/3 handshake markers all flow through RTT channel 0.

**SEGGER J-Link RTT Viewer** is the day-to-day host tool we use to watch this stream live. It's a small standalone GUI shipped with the J-Link Software pack: launch it, select the target device, point it at the RTT control-block address, and the active RTT channels render as plain-text terminals in real time. It supports multiple channels in parallel — channel 0 for application logs, channel 1 for high-volume trace, channel 2 for binary captures — each in its own tab. Because RTT Viewer attaches to the running J-Link session without halting the CPU, you can leave it open across hours of testing and never miss a log line. For unattended captures we instead use **`JLinkRTTLogger`** from the command line; same protocol, no GUI, writes the channel directly to a file (`JLinkRTTLogger -rttchannel 0 -rttsearchranges "0x38000000 0x10000" log.txt`).

On this project the RTT control block and ring buffer sit in D3 SRAM4 (`0x38000000`) — completely outside the main AXI memory where the application's data lives. J-Link's RAM-scan finds the `"SEGGER RTT"` magic signature there at session start, and from then on the RTT channel is "free" in the sense that turning it off would not give us a faster boot or a roomier AXI.

### `cspybat` — command-line C-SPY for automated probes

`cspybat` is IAR's headless invocation of the C-SPY debug engine. It loads a `.out` file, attaches the configured probe (J-Link, in our case), runs a macro file, and exits — all from a shell prompt or batch script. The killer use case is **automated post-build verification**: every time the firmware is built, a CI step (or a developer's `.bat` file) can run a probe that flashes the binary, lets it boot, halts at a known function, dumps the relevant peripheral registers and global variables, and writes a log file the developer reads. No human in the loop. On this project we have probe scripts for boot-stage verification (25 checkpoints across `MX_*_Init`), PFB strip dispatch verification, LTDC/DSI pipeline state, and DFSDM register snapshot capture at button-press EXTI. Each one is a one-line invocation of `cspybat.exe ... --macro=probe.mac > log.txt`. The boot probe alone has caught a dozen regressions over the project's lifetime — clock-tree misconfigurations, MPU regions that drifted, peripheral init order that broke after a CubeMX regenerate — all without anyone opening the IAR IDE.

### C-SPY macros — programmable debugger logic

The C-SPY macro language is the scripting layer that sits inside `cspybat` (or the interactive IDE). A `.mac` file can install breakpoints by symbol name, read C variables and struct fields directly (the debugger resolves symbols from the `.out`/ELF, you don't grep the `.map`), write to peripheral registers, advance the CPU between checkpoints, and emit formatted text to the log. The canonical pattern is `__setCodeBreak("MX_LTDC_Init", 0, "1", "TRUE", "onHit()")`, where `onHit()` is a macro function that dumps whatever state matters at that breakpoint and the session continues. For multi-checkpoint flows we use `__hwRunToBreakpoint(&main, 5000)` to chain advances.

The macro layer is what makes C-SPY genuinely powerful versus a generic GDB session — symbol-aware breakpoints survive a rebuild (no address re-extraction), struct-field reads work without manual offset arithmetic, and the same `.mac` file can be replayed in the IDE for interactive use. There is one specific quirk to be aware of on this project: with passive `__setCodeBreak`, only the first breakpoint hit reliably runs its action under `cspybat`; for multi-BP flows we use an orchestrator (PowerShell) that runs `cspybat` once per breakpoint, substituting the target symbol via a template `.mac.tpl` file. The active `__hwRunToBreakpoint` flow doesn't have this limit.

> **For a deeper walkthrough of `cspybat` command syntax, the C-SPY macro language reference, the lifecycle hooks (`execUserSetup`, `execUserExit`, `execUserExecutionStopped`), the one-useful-BP-per-session quirk, and worked probe examples from this project — including boot-stage verification, LTDC pipeline checkpoints, and DFSDM register snapshots — see my dedicated post: [Debugging STM32 with cspybat and C-SPY Macros — A Practical Guide](TODO-ADD-SIGHTSYS-BLOG-LINK) on the Sightsys website.**

### Choosing the right tool for the question

The three tools answer different questions and don't substitute for each other. **RTT** answers "what is the firmware doing right now?" — continuous narrative trace. **`cspybat` + C-SPY macros** answer "what is the state at this specific symbol when execution reaches it?" — symbol-resolved snapshots. **J-Link Commander** (a fourth tool we use but didn't expand on here) answers "what is in this memory address while the target keeps running?" — free-running memory reads with no halt. Together they replace `printf` debugging entirely. On a 400 MHz Cortex-M7 running a partial-framebuffer LCD pipeline alongside live audio capture, `printf` was never going to be the right answer. RTT plus C-SPY plus J-Link memory probing is.

---

## 12. References

### Official STMicroelectronics documentation (all available on st.com)

All of the application notes (AN) and reference manuals (RM, UM) below are published by STMicroelectronics on their official website (`www.st.com`). Each document is downloadable for free; search the document number on the ST site to get the latest revision.

- **AN4861 — LCD-TFT display controller (LTDC) on STM32 MCUs** (STMicroelectronics application note). FUIF / FIFO underrun causes, framebuffer bandwidth math, recommended memory placement for LTDC scan-out. Available at `www.st.com`.
- **AN4891 — STM32H7 system architecture and performance** (STMicroelectronics application note). The canonical reference for the H7's bus matrix, the three power domains, and DMA-master reach across regions. Available at `www.st.com`.
- **AN5215 — DMA controllers on STM32H7 MCUs** (STMicroelectronics application note). Memory bandwidth, performance numbers, master arbitration. Available at `www.st.com`.
- **AN5405 — Managing memory protection unit in STM32 MCUs** and the related cache-coherency guidance (STMicroelectronics application note). Required reading for the Cortex-M7 D-cache + DMA interaction described in Section 9. Available at `www.st.com`.
- **AN5027 — Interfacing PDM digital microphones using STM32 MCUs** (STMicroelectronics application note). PDM clock and decimation pairing, mic-clock physics, AN5027 §2.4.2 on the shared-source rule. Available at `www.st.com`.
- **RM0399 — STM32H745/755 and STM32H747/757 advanced Arm-based 32-bit MCUs reference manual** (STMicroelectronics reference manual). Chapter 2 (memory map), chapter 8 (RCC), the bus-matrix appendix, and chapter 60 (DBGMCU) are the relevant sections for this post. Available at `www.st.com`.
- **UM2411 — STM32H7 SAI (serial audio interface)** (STMicroelectronics user manual). PDM input mode, SAI4 register-level detail. Available at `www.st.com`.

### Tool-vendor documentation

- **SEGGER RTT documentation** (SEGGER Microcontroller) — wire protocol, control-block layout, host tools. Available at `www.segger.com`.
- **SEGGER J-Link / J-Trace User Guide (UM08001)** (SEGGER Microcontroller) — J-Link Commander commands, SWO, RTT, scripted batch invocation. Available at `www.segger.com`.
- **IAR C-SPY Debugging Guide (UCSARM-26)** (IAR Systems) — macro language reference, cspybat invocation, lifecycle hooks. Available at `www.iar.com`.
- **IAR Linker and Library Tools Reference Guide** (IAR Systems) — `.icf` linker-config syntax, section placement, `#pragma location` semantics. Available at `www.iar.com`.

For the IAR-specific compiler syntax (`#pragma location`, `@` placement operator, `.icf` directives), the **IAR C/C++ Development Guide** ships with every EWARM installation under the `doc/` subdirectory.

---

## 13. Conclusion

Five rules cover most of the placement decisions on STM32H7:

1. **Pick the domain that owns the master that uses the buffer.** DFSDM in D2 means audio buffers in D2 SRAM. LTDC in D1 means framebuffer in D1 AXI.
2. **Enforce strict static allocation.** Heap-allocated objects move when the heap moves; static objects have fixed addresses you can audit in the `.map` and inspect via C-SPY without instrumentation.
3. **Use mandatory IAR placement — `#pragma location`, the `@` operator, or section attributes** — for every critical variable. Never let a critical buffer float in default `.bss` or `.data`.
4. **Minimise cross-domain bus traffic.** Each domain serves one traffic class. Heap and DMA buffers don't share a region. UART DMA buffer and its consumer queue stay in the same domain.
5. **Adopt zero-footprint debugging — ITM, SWO, and `cspybat` C-SPY macros** — instead of `printf`. At 480 MHz, `printf` alters the system under test; ITM doesn't.

Two cautions sit alongside the rules. Always define explicit MPU regions for every memory area accessed by DMA, marking them Bufferable / Non-Cacheable; without that, the CPU cache and the DMA controller will hold conflicting versions of the same data, and you'll spend days hunting non-deterministic corruption. And always use Mutexes (not binary semaphores) for shared resources in a multitasking system, because Mutexes support priority inheritance and prevent lower-priority tasks from inadvertently blocking higher-priority ones.

The practical takeaway: memory placement on H7 is part of the design, not part of the optimisation pass. Treat each new buffer like you treat each new task — give it a name, a domain, a reason, and a verified location in the `.map` — and the worst class of "works on Tuesday, breaks on Wednesday" bugs goes away.

---

## 14. Related Reading on the Sightsys Engineering Blog

If you found this STM32H7 memory architecture deep-dive useful, you'll likely find the following companion posts on the Sightsys blog directly relevant:

- **[Debugging STM32 with cspybat and C-SPY Macros — A Practical Guide](TODO-ADD-CSPYBAT-BLOG-LINK)** — the full toolchain walk-through summarised in Section 11 of this post. Covers macro language syntax, lifecycle hooks, the IAR project setup, and worked examples of automated post-build probes.
- **[TouchGFX Partial Framebuffer on STM32H7 — Eliminating LCD Tearing and FUIF](TODO-ADD-PFB-BLOG-LINK)** — the LCD pipeline architecture mentioned in Sections 5 and 7 of this post. Covers the 4-strip dispatch, DMA2D scheduling, and the DSI Command Mode integration.
- **[FreeRTOS on Dual-Core STM32H747 — Task Priorities, Static Allocation, and the Bus Matrix](TODO-ADD-FREERTOS-BLOG-LINK)** — the RTOS side of the architecture covered briefly in Section 4 of this post. Static-queue migration, priority inversion, and the bus matrix's effect on task scheduling.

---

## 15. Call to Action

If you're building an STM32H7 product and hitting any of the symptoms described in Section 1 — audio jitter, LCD tearing, silent DMA corruption, FreeRTOS queue weirdness — **the Sightsys engineering team works on this class of problem for a living**. We help product teams from start-ups to OEMs design memory architectures, debug bus-contention issues, integrate TouchGFX cleanly, and bring STM32H7 designs to production.

**[Get in touch with Sightsys →](TODO-ADD-CONTACT-LINK)**

Or subscribe to the Sightsys engineering blog (link at the top of the page) for more deep-dives on STM32, embedded debugging, and real-time architecture.

---

*Built and debugged with IAR Embedded Workbench 9.70.2 (`cspybat` 9.4.6.1706), SEGGER J-Link V9.30, on the STM32H747I-DISCO board.*

*Author: [your name here], Sightsys engineering. Estimated reading time: 18-22 minutes.*
