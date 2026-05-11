# Bug — PCM stream to NORA times out at ~75% (72,419 / 96,512 bytes)

**Filed:** 2026-05-11
**Branch:** `test/inject-flasher-thumbnail-no-wifi`
**HEAD at time of bug:** static-queue migration + section attribution (post `39beb8d`)
**Severity:** Blocks STT — voice never reaches GCS, no transcript possible
**Status:** Open

---

## Symptom

After successful UART handshake fix (xRawBleQueue → static + AXI placement), the STM32→NORA WAV stream now starts but stalls partway:

```
NORA log:
I (224495) NORA: AUDIO:FILE 'REC_006.wav' 96512 bytes — streaming to GCS
I (224495) cloud_upload: StreamWav: AUDIO:READY sent — buffering 96512 bytes from UART
E (235985) cloud_upload: StreamWav: UART timeout at 72419/96512   ← 11.5 s later
```

STM32 reported `STREAM2 PASS` (handshake OK) and started transmitting from `audio_sd.c::AudioSD_StreamFile()`, but NORA's 5-second inter-chunk timeout fired before all 96,512 bytes arrived.

**Rate observed:** 72,419 B / 11.5 s ≈ **6.3 KB/s effective**
**Rate expected:** ~50 KB/s (1024-byte chunks at 921,600 baud + 10 ms pacing)
**Ratio:** ~8× slower than design

---

## Repro

1. Boot HEAD with `AUDIO_DEBUG_LCD_DISABLED 1`, NORA on iPhone hotspot
2. Wait for NORA to discover PC and announce ready
3. Press blue PC13 button on STM32, speak (volume doesn't matter for this bug)
4. STM32: SD write completes (`STREAM2 PASS` after ~24 s)
5. NORA: `AUDIO:READY` sent
6. STM32 begins streaming the 96 KB WAV via UART8 TX
7. After ~11.5 s, NORA logs `UART timeout at 72419/96512`

100% reproducible on the current build (observed in test session 2026-05-11 17:xx).

---

## Implementation location

[CM7/Core/Src/audio_sd.c:485-538](CM7/Core/Src/audio_sd.c#L485-L538) — `AudioSD_StreamFile()` stream loop:

```c
#define AUDIO_SD_UART_CHUNK  1024u    // line 71

static uint8_t s_txChunk[AUDIO_SD_UART_CHUNK] __attribute__((aligned(32)));   // line 485
while (1)
{
    res = f_read(&file, s_txChunk, sizeof(s_txChunk), &br);                  // line 493
    if (res != FR_OK)  { ... break; }
    if (br == 0) break;

    HAL_StatusTypeDef txRes = HAL_ERROR;
    for (int attempt = 0; attempt < 3; attempt++) {                          // line 508
        txRes = HAL_UART_Transmit(&huart8, s_txChunk, (uint16_t)br, 500);
        if (txRes == HAL_OK) break;
        retryTotal++;
        ...
        vTaskDelay(pdMS_TO_TICKS(5));   // line 519 - back-off
    }
    if (txRes != HAL_OK) { ok = false; break; }
    sentTotal += br;

    vTaskDelay(pdMS_TO_TICKS(10));   // line 537 - inter-chunk pacing
}
```

NORA-side inter-chunk timeout (5 s) is in [cloud_upload.c::StreamWav](NORA_BLE/nora_wifi_ble_translate/main/cloud_upload.c).

---

## Suspected causes (in priority order)

### 1. Inter-chunk pacing delay too long for current SDXC card
Math at design intent:
- 1024 B × 11 ms TX time (921600 baud, 10 bits/byte) ≈ 11 ms per chunk on the wire
- + 10 ms `vTaskDelay` per chunk
- + `f_read` of 1024 B from SDXC card
- 96 chunks × (~21 ms + read time)

If the 64 GB SDXC takes ~50-80 ms per 1024 B read (slower than the older SDHC card from earlier sessions), each chunk is ~80-100 ms total → 96 chunks × 90 ms ≈ **8.6 s**, close to observed.

**Smallest fix:** drop `vTaskDelay(pdMS_TO_TICKS(10))` → `pdMS_TO_TICKS(2)` and increase chunk size 1024 → 4096 B to amortize `f_read` overhead.

### 2. SD card read latency on the 64 GB SDXC
SDXC cards (≥32 GB, exFAT) often have higher random-read latency than SDHC due to wear-leveling and larger Flash translation layer. Reading 1 KB at a time may be hitting an FTL miss every chunk.

**Mitigation:** larger chunks (4-8 KB) batch the read; FTL hits less often.

### 3. HAL_UART_Transmit blocking on huart8 contention
If `xRawBleQueue` consumer (UARTReceiveTask, priority 26) preempts the SDWriteTask (priority 20) at every received byte from NORA's heartbeat, the stream loop may not get scheduled smoothly. The 500 ms HAL TX timeout combined with retry back-offs could compound.

**Check:** read `g_UartHealth.tx_retries` / `tx_last_err_code` after the next failed run.

### 4. NORA-side timeout too aggressive
The 5 s timeout assumes ~20 KB/s minimum throughput. With slower-than-expected delivery, NORA gives up before STM32 finishes.

**Quickest unblock:** bump NORA's inter-chunk timeout from 5 s → 15 s while we work on the STM32 throughput.

---

## Test plan — second iteration (this session)

Apply **smallest fix first** (cause #1 + #2):

1. **`AUDIO_SD_UART_CHUNK`**: 1024 → **4096** bytes (line 71 of audio_sd.c)
2. **`vTaskDelay(pdMS_TO_TICKS(10))`**: 10 → **2** ms (line 537)

Expected effect:
- 96512 B / 4096 = 24 chunks (was 96)
- Each chunk: ~45 ms TX + 2 ms delay + ~50 ms read = ~100 ms
- Total: ~2.4 s — well under NORA's 5 s timeout, even with SD latency variability

If this passes (NORA reports `streaming 96512/96512`), proceed to Phase 3 (4× gain in `StoreDmaChunk`). If still fails, instrument with chunk-level RTT timing to find the actual bottleneck.

---

## Diagnostic instrumentation to add

After the first retest, if it still fails:

1. Add `RLOG_TS("[STREAM] chunk %u sent at +%u ms", chunkIdx, HAL_GetTick() - t0)` every 16 chunks
2. Log `g_UartHealth.tx_retries` and `huart8.ErrorCode` in the final `STREAM_DONE_BYTES=` line
3. Add ITM marker before/after `f_read` to measure SD read latency

---

## Related history

- Earlier today: same code path WORKED end-to-end (commit `745de5d` message says "GCS upload HTTP 200, STT called"). Today's regression coincides with switching to the **64 GB SDXC card**.
- The static-queue migration that fixed the AUDIO:READY handshake did not touch the stream loop.
- `STREAM_RETRIES` and `tx_hard_fails` counters exist (line 489, 524) but weren't visible in the recent log — extract them from the next failed run.

---

## References

- [audio_sd.c:421-558](CM7/Core/Src/audio_sd.c#L421-L558) — full `AudioSD_StreamFile` implementation
- [CLAUDE.md Hard Rules #1 + #3](CLAUDE.md) — static allocation + section attribution
- [docs/MEMORY_MAP_AND_SW_BLOCKS.md](docs/MEMORY_MAP_AND_SW_BLOCKS.md) — current memory layout
