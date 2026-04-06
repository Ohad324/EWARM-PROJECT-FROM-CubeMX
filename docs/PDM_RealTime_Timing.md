# PDM Audio Pipeline — Real-Time Deadline Analysis

## The Core Principle

To understand if the system is stable, look at the **Real-Time Deadline**.
In audio processing, the DMA is the "clock" that everyone else must follow.
If the CPU takes longer to process the filter than the DMA takes to fill the
buffer, you get an **Overrun** — lost data, corrupted audio.

---

## 1. DMA Time — The Hard Deadline

The DMA fill time is fixed by sample rate and buffer size. It is independent
of CPU speed; it only depends on the audio clock.

| Parameter | Value |
|-----------|-------|
| Sample rate | 16,000 Hz |
| Samples per DMA half-buffer | 16 (= `output_samples_number`) |

```
Time = Samples / Sample Rate = 16 / 16,000 = 0.001 s = 1 ms
```

**The DMA fires every 1 ms.** The CPU has a strict 1 ms deadline to finish
its work before the next half-complete or full-complete interrupt fires.

---

## 2. CPU Processing Time

### A. PDM2PCM Filter (the fast part)

For 16 output samples, `PDM_Filter()` using the ST high-performance library
(Cortex-M7 DSP instructions, 480 MHz):

| Metric | Value |
|--------|-------|
| Clock cycles | ~3,000 – 5,000 cycles |
| Time at 480 MHz | 5,000 / 480,000,000 ≈ **10 µs** |
| % of 1 ms deadline | **~1%** |

The filter itself is not the problem.

### B. Hidden Costs — SD Card and OS Overhead

| Operation | Typical time | Worst case |
|-----------|-------------|------------|
| FreeRTOS context switch | ~1 µs | ~5 µs |
| SD card write (normal) | 200 – 500 µs | — |
| SD card write (busy / GC) | — | 1,000 – 5,000 µs |

SD card internal flash management ("garbage collection" or wear-levelling)
can cause a single write to stall for 10 ms or more — 10× over the deadline.

---

## 3. Why the Queue-Based Drain Saves You

Because the DMA fires every 1 ms and the filter takes only 0.01 ms, there
is a large safety margin (0.99 ms) under normal conditions.

**Old design (single semaphore + ping-pong counter):**

If an SD card write took 1.5 ms (just over the deadline), the next DMA
interrupt fired while the CPU was still blocked. The semaphore accumulated
two tokens. When the task finally woke, it incremented `g_DmaCallCount`
twice in quick succession. Because both tokens looked identical, the counter
lost track of which half was which — the CPU processed the same 16-sample
block twice → **repeating 16-sample pattern → 500 Hz square wave → BEEP**.

**New design (pointer queue):**

Each DMA callback posts the exact buffer address (`&g_PdmBuf[0]` or
`&g_PdmBuf[PDM_BUF_HALF]`) into a FreeRTOS queue. The drain loop pops
entries in FIFO order. If an SD write took 1.5 ms and two callbacks fired,
the queue holds:

```
[ &g_PdmBuf[0], &g_PdmBuf[64] ]   ← in arrival order
```

The task wakes, pops `&g_PdmBuf[0]`, runs the filter (10 µs), pops
`&g_PdmBuf[64]`, runs the filter again (10 µs). Total catch-up: 20 µs.
The correct half is always processed — no counter drift, no repeat.

---

## 4. Summary Table

| Operation | Time | Status |
|-----------|------|--------|
| DMA fill (½ buffer) | 1,000 µs | **Hard deadline** |
| PDM filter (16 samples) | ~10 µs | Very safe (1% of budget) |
| SD card write (typical) | 200 – 500 µs | Safe |
| SD card write (worst case) | 1,000 – 5,000 µs | Danger zone |
| Queue catch-up (2 missed halves) | ~20 µs | Instant recovery |

**The CPU is ~100× faster than the DMA for the filtering step.**
The only thing that can break the ping-pong rhythm is a slow SD card —
and the queue design recovers from that automatically.

---

## 5. Measuring Exact CPU Time with DWT

The DWT (Data Watchpoint and Trace) cycle counter is already enabled in this
project (`DWT->CYCCNT`). To measure exactly how long `PDM_Filter()` takes:

```c
/* In voice_recorder.c — StoreDmaChunk(), around the PDM_Filter call */
uint32_t t0 = DWT->CYCCNT;
PDM_Filter((uint8_t *)pdmSrc, (void *)s_pcmHalf, &s_pdmHandler);
uint32_t cycles = DWT->CYCCNT - t0;
/* cycles / 480 = microseconds */
```

Log it with:
```c
RLOG("[PDM] filter cycles=%lu  time=%lu us", cycles, cycles / 480u);
```

Run it for one recording and check the max value — anything under 480,000
(1 ms × 480 MHz) is within budget. Typical values are 3,000 – 5,000 cycles
(6 – 10 µs).
