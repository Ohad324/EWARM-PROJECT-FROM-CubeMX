# STM32H747I-DISCO — SW Block Diagram & Memory Placement Audit

**Generated:** 2026-05-11
**Source of truth:** `EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map`
**Linker config:** `EWARM/stm32h747xx_flash_CM7.icf`
**Active branch:** `test/inject-flasher-thumbnail-no-wifi`

---

## 1. Memory Domains (STM32H747 silicon)

```
┌──────────────────────────────────────────────────────────────────────────────┐
│  D1 — CORE DOMAIN (always-on while M7 runs)                                  │
│  ────────────────────────────────────────────────                            │
│  CPU:    Cortex-M7 @ 400 MHz                                                 │
│  DMA:    MDMA (all domains), DMA2D (D1 only)                                 │
│  Bus:    AXI / AHB1 / AHB3                                                   │
│  RAM:                                                                        │
│    • Flash      0x08000000  (1 MB)     code, .intvec, const tables           │
│    • ITCM       0x00000000  (64 KB)    UNUSED (no DMA reach)                 │
│    • DTCM       0x20000000  (128 KB)   CSTACK (8 KB), HEAP (1 KB) - NO DMA   │
│    • AXI SRAM   0x24000000  (512 KB)   ALL FreeRTOS tasks, framebuffer,      │
│                                          heap_4 (96 KB), HAL handles,        │
│                                          UART/queue/mutex CBs                │
└──────────────────────────────────────────────────────────────────────────────┘
                        │ AHB crossbar (CM7 can reach D2/D3 RAM)
┌──────────────────────────────────────────────────────────────────────────────┐
│  D2 — PERIPHERAL DOMAIN                                                      │
│  ────────────────────────────────                                            │
│  CPU:    Cortex-M4 @ 200 MHz (NOT used — CM4 stub only at 0x08100000)        │
│  DMA:    DMA1 (audio: DFSDM→SRAM1), DMA2 (SD: SDMMC1 IDMA bounce)            │
│  Bus:    AHB / APB1 / APB2                                                   │
│  Peripherals: DFSDM1, SDMMC1, SAI1-3, UART8, USART1-3, SPI, I2C              │
│  RAM:                                                                        │
│    • SRAM1   0x30000000  (128 KB)   s_DfsdmBuf @ 0x30004000  [512 B DMA ring]│
│    • SRAM2   0x30020000  (128 KB)   g_AudioBuf @ 0x30020000  [96 KB PCM]     │
│    • SRAM3   0x30040000  (32 KB)    UNUSED                                   │
│  Reach: DMA1/DMA2 can access D1 AXI + D2 SRAM (NOT D3)                       │
└──────────────────────────────────────────────────────────────────────────────┘
                        │ internal silicon bridge (SAI4→DFSDM1 — N/A on this branch)
┌──────────────────────────────────────────────────────────────────────────────┐
│  D3 — LOW-POWER DOMAIN                                                       │
│  ────────────────────────                                                    │
│  CPU:    none                                                                │
│  DMA:    BDMA — ONLY reaches D3 SRAM4                                        │
│  Peripherals: SAI4 (gated off — Path C-PC2 uses DFSDM-master),               │
│               LPTIM, RTC, LPUART1, ADC3, DAC, I2C4, SPI6                     │
│  RAM:                                                                        │
│    • SRAM4   0x38000000  (64 KB)    UNUSED in this project                   │
│    • Backup  0x38800000  (4 KB)     UNUSED                                   │
└──────────────────────────────────────────────────────────────────────────────┘
```

---

## 2. SW Component Block Diagram (audio pipeline + UART bridge)

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                          STM32H747 CM7 — VOICE RECORDER                      ║
╚══════════════════════════════════════════════════════════════════════════════╝

   ┌──── HARDWARE ─────┐                  ┌──── HARDWARE ─────┐
   │  MP34DT05-A mic   │                  │  NORA-W106 ESP32  │
   │  PDM data on PC1  │                  │  UART8 @ 921600   │
   │  CLK from PC2     │                  │  Wi-Fi → GCS/STT  │
   └─────┬─────────────┘                  └────────┬──────────┘
         │ PDM 2 MHz                               │ UART8 TX/RX
         ▼                                         ▼
   ┌──────────────────┐                  ┌──────────────────────┐
   │ DFSDM1_Channel0  │                  │      UART8 HAL       │
   │  (clock master + │                  │  (DMA1_Stream0 RX,   │
   │   data sampler)  │                  │   blocking TX)       │
   └─────┬────────────┘                  └────────┬─────────────┘
         │ Sinc3, OSR=125                         │ ISR + DMA-IDLE
         │ → 16 kHz PCM24                         │
         ▼                                        ▼
   ┌────────────────────────────┐       ┌────────────────────────────┐
   │     DMA1_Stream1           │       │     DMA1_Stream0           │
   │  (DFSDM1_Filter0 → SRAM1)  │       │  (UART8_RX → AXI SRAM)     │
   │  s_DfsdmBuf @ 0x30004000   │       │  s_dma_rx_buf @ 0x2406EEC0 │
   │  128 × int32, circular     │       │  128 bytes, normal mode    │
   │  MPU: Non-Cacheable,Share  │       │  D-Cache enabled (invalid. │
   │  D2 SRAM1                  │       │  on RX complete)           │
   └─────────────┬──────────────┘       └─────────────┬──────────────┘
                 │ DMA HT/TC ISR                      │ IDLE ISR fires
                 ▼                                    ▼
   ┌────────────────────────────┐       ┌────────────────────────────┐
   │  HAL_DFSDM_Filter…Cplt-     │       │ HAL_UARTEx_RxEventCallback │
   │   Callback (ISR)            │       │  (UART8_IRQHandler)        │
   │  → store via StoreDmaChunk  │       │  → SCB_InvalidateDCache    │
   │     (CPU >>8 shift)         │       │  → memcpy to BleRawMsg_t   │
   └─────────────┬──────────────┘       │  → xQueueSendFromISR       │
                 │ writes int16          │     into xRawBleQueue      │
                 ▼                       └─────────────┬──────────────┘
   ┌────────────────────────────┐                     │ 4-slot queue
   │   g_AudioBuf @ 0x30020000  │                     ▼
   │   48000 × int16 (96 KB)    │       ┌────────────────────────────┐
   │   D2 SRAM2                 │       │   xRawBleQueue (static)    │
   │   MPU: Non-Cacheable,Share │       │   Handle  @ 0x2406FBDC     │
   └─────────────┬──────────────┘       │   CB      @ 0x2406F2C4     │
                 │                       │   Storage @ 0x2407ACB4     │
                 │                       │   4 × BleRawMsg_t (520 B)  │
                 │                       │   AXI SRAM                 │
                 │                       └─────────────┬──────────────┘
                 │                                     │
                 ▼                                     ▼
╔══════════════════════════════ FreeRTOS TASKS ════════════════════════════════╗

   ┌──────────────────────┐      ┌──────────────────────┐
   │  VoiceRecTask        │      │  UARTReceiveTask     │
   │  Priority 32         │      │  Priority 26         │
   │  Stack 3072 W (AXI)  │      │  Stack 1024 W (AXI)  │
   │                      │      │                      │
   │  - Drain DFSDM DMA   │      │  - Recv from queue   │
   │  - Run 3-sec window  │      │  - Parse AUDIO:READY │
   │  - Signal SDWrite    │      │  - Notify SDWrite    │
   └──────────┬───────────┘      └──────────┬───────────┘
              │ xVoiceQueue                 │ task notify
              ▼                             ▼
   ┌────────────────────────────────────────────────────┐
   │  SDWriteTask  (Priority 20, Stack 2048 W in AXI)   │
   │  - Open REC_XXX.wav, write header                  │
   │  - Bounce g_AudioBuf via s_pcmBounce (AXI)         │
   │  - SDMMC1 IDMA writes to SD card                   │
   │  - Then: send "AUDIO:FILE name SIZE" via UART8 TX  │
   │  - Wait for "AUDIO:READY" notification             │
   │  - Stream 96 KB PCM over UART8 TX                  │
   └──────────────────┬─────────────────────────────────┘
                      │
                      ▼
   ┌──────────────────────┐      ┌──────────────────────┐
   │  VoiceCMDhandler     │      │  RTTLogTask          │
   │  Priority 16         │      │  Priority 1          │
   │  Stack 1536 W (AXI)  │      │  Stack 1024 W (AXI)  │
   │  - Process commands  │      │  - Drain xLogQueue   │
   │    from xRawBleQueue │      │  - Print to RTT + IO │
   └──────────────────────┘      └──────────────────────┘
```

---

## 3. Memory Placement Audit (current `.map`)

### D1 AXI SRAM `0x24000000 .. 0x2407FFFF` — 512 KB

| Range | Size | Symbol / Owner | Domain | Notes |
|---|---|---|---|---|
| `0x24000000` – `0x24046500` | 281 KB | `TouchGFX_Framebuffer` (PFB strip) | D1 AXI | `FB_BLOCK`, 32-aligned, LTDC scan target |
| `0x24046500` – `0x24046734` | ~564 B | `.data` (init data) | D1 AXI | RTT, OS wrappers, locale data |
| `0x24046734` – `0x240467AC` | 120 B | `s_hdma_uart8_rx` (DMA handle) | D1 AXI | ble_uart.o |
| `0x240467AC` – `0x24050000` | ~38 KB | TouchGFX engine, JPEG, BSP, FatFS .bss | D1 AXI | task globals |
| `0x24050000` – `0x24050800` | 2 KB | `g_irq_log` (IRQ ring, 256×8 B) | D1 AXI | stm32h7xx_it.o |
| `0x24050800` – `0x24050804` | 4 B | `g_irq_idx` | D1 AXI | running count |
| `0x24050804` – `0x24068EA4` | **96 KB** | **`ucHeap`** (FreeRTOS heap_4 default) | D1 AXI | heap_4 internal xHeap |
| `0x24068EA4` – `0x2406EEC0` | ~24 KB | rtos_trace bufs + misc | D1 AXI | |
| `0x2406EEC0` – `0x2406EF40` | 128 B | **`s_dma_rx_buf`** ⭐ | D1 AXI | UART8 RX DMA target |
| `0x2406EFBC` – `0x2406F034` | 120 B | `s_hdma_dfsdm` | D1 AXI | DFSDM DMA handle |
| `0x2406F034` – `0x2406F0AC` | 120 B | `hdma_sai4_a_rx` | D1 AXI | (unused but linked) |
| `0x2406F25C` – `0x2406F2C4` | 104 B | `hdma2d` + misc HAL handles | D1 AXI | |
| `0x2406F2C4` – `0x2406F324` | **96 B** | **`s_rawBleQueueCB`** ⭐ NEW | D1 AXI | `StaticQueue_t` |
| `0x2406F324` – `0x2406F384` | **96 B** | **`s_bleHistMutexCB`** ⭐ NEW | D1 AXI | `StaticSemaphore_t` |
| `0x2406F55C` – `0x2406F844` | ~744 B | `hdfsdm1_filter0`, `hdfsdm1_channel0` | D1 AXI | DFSDM HAL handles |
| `0x2406FBD8` – `0x2406FC88` | 176 B | `xBleHistMutex`, `xRawBleQueue`, `xLogQueue`, `xVoiceQueue`, `xMusicQueue`, `g_pfb_dbg_*` | D1 AXI | queue/mutex handles + debug counters |
| `0x2407ACB4` – `0x2407AEBC` | **520 B** | **`s_rawBleQueueStorage`** ⭐ NEW | D1 AXI | 4 × `BleRawMsg_t` (132 B each) |
| `0x2407C2C0` – `0x2407C4C0` | 512 B | `UARTReceiveTask::accum` | D1 AXI | UART line accumulator |
| `0x2407C640` – `0x2407C780` | 320 B | `bleHistory` (10×32) | D1 AXI | UI history grid |
| `0x2407C960` – `0x2407C9F4` | 148 B | `huart8` (HAL handle) | D1 AXI | |
| `0x2407CDE8` – `0x2407FFFF` | ~13 KB | **FREE** | — | unused tail |

⭐ = new symbols introduced 2026-05-11 (static-queue migration + section attribution).

### D1 DTCM `0x20000000 .. 0x2001FFFF` — 128 KB

| Range | Size | Symbol | Domain | Notes |
|---|---|---|---|---|
| `0x20000000` – `0x20002000` | 8 KB | `CSTACK` (M7 main + ISR stack) | D1 DTCM | NO DMA reach |
| `0x20002000` – `0x20003000` | 4 KB | `HEAP` (C lib heap, unused) | D1 DTCM | NO DMA reach |
| `0x20003000` – `0x2001FFFF` | ~116 KB | **FREE** | — | unused |

### D2 SRAM1 `0x30000000 .. 0x3001FFFF` — 128 KB

| Range | Size | Symbol | Domain | Notes |
|---|---|---|---|---|
| `0x30000000` – `0x30004000` | 16 KB | **FREE** | — | unused |
| `0x30004000` – `0x30004200` | 512 B | **`s_DfsdmBuf`** | D2 SRAM1 | DFSDM1 DMA ring (`#pragma location`) |
| `0x30004200` – `0x3001FFFF` | ~112 KB | **FREE** | — | unused |

### D2 SRAM2 `0x30020000 .. 0x3003FFFF` — 128 KB

| Range | Size | Symbol | Domain | Notes |
|---|---|---|---|---|
| `0x30020000` – `0x30037700` | **96 KB** | **`g_AudioBuf`** | D2 SRAM2 | 48000 × int16 PCM, `#pragma location` |
| `0x30037700` – `0x3003FFFF` | ~34 KB | **FREE** | — | unused |

### D3 SRAM4 `0x38000000 .. 0x3800FFFF` — 64 KB

| Range | Owner | Notes |
|---|---|---|
| ENTIRE 64 KB | **UNUSED** | SAI4 retired in Path C-PC2; BDMA not used |

### External SDRAM `0xD0000000 .. 0xD1FFFFFF` — 32 MB

| Range | Size | Symbol | Domain | Notes |
|---|---|---|---|---|
| `0xD0000000` – `0xD02A3000` | 2.6 MB | `s_rgb888Buf` | SDRAM | JPEG decode intermediate |
| `0xD02A3000` – `0xD0465000` | 1.7 MB | `s_ycbcrBuf` | SDRAM | JPEG MCU output |
| `0xD0465000` – `0xD057E400` | 1.1 MB | `s_rgb888Scaled` | SDRAM | scaled 800×480 RGB888 |
| `0xD05B0400` – `0xD1FFFFFF` | ~26 MB | **FREE** | — | unused tail |

---

## 4. Collision Audit

### Check 1 — Audio buffers in their own domain

| Buffer | Address | Domain | DMA controller | Status |
|---|---|---|---|---|
| `s_DfsdmBuf` | `0x30004000` | D2 SRAM1 | DMA1_Stream1 | ✓ DMA1 can write D2 SRAM1 |
| `g_AudioBuf` | `0x30020000` | D2 SRAM2 | CPU writes only | ✓ Reachable from CPU |
| Gap between them | 124 KB | — | — | ✓ No overlap, no neighbor |

### Check 2 — UART RX pipeline same domain

| Buffer | Address | Domain | Path | Status |
|---|---|---|---|---|
| `s_dma_rx_buf` | `0x2406EEC0` | D1 AXI | DMA1_Stream0 RX target | ✓ DMA1 can write D1 AXI |
| `s_rawBleQueueCB` | `0x2406F2C4` | D1 AXI | ISR `xQueueSendFromISR` writes | ✓ Same domain as DMA buf |
| `s_rawBleQueueStorage` | `0x2407ACB4` | D1 AXI | 4-slot ring buffer | ✓ Same domain |
| `s_bleHistMutexCB` | `0x2406F324` | D1 AXI | Mutex CB | ✓ CPU-only access |

**No cross-domain bus traffic per RX message.**

### Check 3 — Framebuffer isolated from audio

| Buffer | Address | Domain | Master | Status |
|---|---|---|---|---|
| `TouchGFX_Framebuffer` (PFB) | `0x24000000` | D1 AXI | LTDC | ✓ |
| `s_DfsdmBuf` | `0x30004000` | D2 SRAM1 | DMA1 | ✓ Different domain — no bus contention |
| `g_AudioBuf` | `0x30020000` | D2 SRAM2 | CPU | ✓ Different domain |

**LTDC scans AXI; audio lives in D2. Eliminates FUIF bandwidth contention.**

### Check 4 — FreeRTOS heap vs DFSDM DMA buffer

| Item | Address | Domain | Notes |
|---|---|---|---|
| `ucHeap` (heap_4) | `0x24050804`, 96 KB | D1 AXI | ✓ Different domain from DFSDM |
| `s_DfsdmBuf` | `0x30004000`, 512 B | D2 SRAM1 | ✓ Isolated |

**Yesterday's bisect concern (heap @ 0x30004200 adjacent to DFSDM buf) is resolved — heap is now in AXI, far away.**

### Check 5 — Stack vs DMA buffers

| Item | Range | Notes |
|---|---|---|
| `CSTACK` (M7) | `0x20000000`–`0x20002000` (DTCM) | ✓ DMA cannot reach DTCM — safe by design |
| Task stacks | inside AXI .bss | ✓ Each task has private stack guarded by FreeRTOS |

### Check 6 — Free space margins

| Domain | Used | Free | Margin |
|---|---|---|---|
| D1 AXI SRAM | ~499 KB | ~13 KB | 2.5% headroom — **tight but OK** |
| D1 DTCM | 12 KB | 116 KB | 90% free |
| D2 SRAM1 | 0.5 KB | 127.5 KB | 99% free |
| D2 SRAM2 | 96 KB | 32 KB | 25% free |
| D3 SRAM4 | 0 | 64 KB | 100% free |
| SDRAM | ~5.4 MB | ~26 MB | 81% free |

**AXI SRAM is the tightest** — if a future feature needs >13 KB additional `.bss` in D1, **move it to D2 SRAM1 or SRAM2** via `#pragma location = ".sram1"` / `".sram2"` (sections defined in `.icf`).

---

## 5. Hard Rules — Where Each Buffer Class Belongs

| Buffer class | Required region | Why |
|---|---|---|
| **LTDC framebuffer** | D1 AXI SRAM (`0x24000000`) | LTDC scans on AHB; AXI removes contention with FMC/SDRAM |
| **DFSDM DMA ring** | D2 SRAM1 (`0x30000000`+) | DMA1 lives in D2; placing in D2 SRAM avoids AXI contention with LTDC |
| **PCM accumulator (`g_AudioBuf`)** | D2 SRAM2 (`0x30020000`+) | CPU writes only, large (96 KB) — keeps D1 AXI free for framebuffer |
| **SDMMC1 IDMA bounce** | D1 AXI SRAM | SDMMC1 IDMA prefers AXI; bounce buffer copied from D2 |
| **UART8 RX DMA buf** | D1 AXI SRAM | DMA1/2 reachable; same domain as ISR-driven queue send |
| **FreeRTOS queues / mutexes / task TCBs** | D1 AXI SRAM (default) | CPU-only access; keep with task stacks |
| **FreeRTOS heap** (`ucHeap`) | D1 AXI SRAM | Default; **never** place adjacent to DMA ring buffers |
| **CSTACK + ISR stack** | DTCM (`0x20000000`) | Zero-wait, NO DMA can corrupt it |
| **JPEG decode intermediates** | External SDRAM | Multi-MB, latency-tolerant |
| **TouchGFX assets** | QSPI Flash (XIP) / SDRAM | Read-only LUTs at runtime |
| **SAI4 BDMA buffers** *(if ever used)* | D3 SRAM4 (`0x38000000`) | BDMA is D3-only; **never** place SAI4-BDMA in D1/D2 |

---

## 6. Section Attribution Cheat Sheet (Hard Rule #3)

```c
/* Default .bss/.data → D1 AXI SRAM (no attribute needed) */
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

/* Absolute address (bypass section regions) — for hardware-fixed buffers */
#pragma location = 0x30004000
static __no_init uint32_t s_DfsdmBuf[128];
```

---

## 7. Open Items / Watch List

1. **AUDIO_DEBUG_LCD_DISABLED 1** — currently disables LCD tasks. Re-enable after STT proven working.
2. **AXI SRAM headroom 2.5%** — monitor for future feature growth; move new buffers to D2 SRAM1/2 if needed.
3. **D3 SRAM4 unused** — available for future BDMA-required peripheral.
4. **JpegDisplayTask kept enabled in pure-PFB-debug mode** — task blocks on empty queue, harmless but redundant.

---

## 8. References

- `EWARM/stm32h747xx_flash_CM7.icf` — linker script (memory regions + section placements)
- `EWARM/STM32H747I-DISCO_CM7/List/STM32H747I-DISCO_CM7.map` — authoritative symbol locations
- `CLAUDE.md` Hard Rule #1 (no dynamic allocation) + Hard Rule #3 (section attribution)
- ST AN4861 (LTDC), AN4891 (system topology), AN5215 (bus bandwidth), AN5405 (cache & coherency)
- RM0399 §2 (memory map), §60.5 (DBGMCU freeze bits)
