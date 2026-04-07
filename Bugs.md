# Bug Log — STM32H747I-DISCO Voice Recorder

## Format
Each entry: ID | Component | Symptom | Root Cause | Fix | Status

---

## B-001 — BUILD: .sdram section missing in .icf
- **Symptom:** Linker error / g_AudioBuf placed in wrong region
- **Root Cause:** ICF linker script missing `.sdram_bss` region definition
- **Fix:** Added SDRAM region to STM32H747xx_flash_CM7.icf
- **Status:** FIXED

---

## B-002 — DMA: vTaskDelay used instead of active drain
- **Symptom:** Audio corrupted / DMA buffer overflow during recording
- **Root Cause:** vTaskDelay(5000) used as recording timer — DMA continued filling buffer while task was suspended
- **Fix:** Replaced with active drain loop (`while g_SampleCount < AUDIO_BUFFER_SAMPLES`)
- **Status:** FIXED

---

## B-003 — SD: f_mount called after xTaskCreate
- **Symptom:** FatFS not ready when first recording attempted
- **Root Cause:** AudioSD_Init() called after scheduler start (from task context), causing timing issues
- **Fix:** Moved AudioSD_Init() to before osKernelStart()
- **Status:** FIXED

---

## B-005/B-006 — SD: DCache invalidation corrupts FatFS winsect
- **Symptom:** FR_INT_ERR / directory corruption during SD write
- **Root Cause:** SCB_InvalidateDCache_by_Addr rounds address down to 32-byte boundary, hitting adjacent FatFS struct fields (winsect before win[])
- **Fix:** Static 32-byte aligned `s_sectorBuf` bounce buffer; DMA never writes directly to caller's buffer
- **Status:** FIXED

---

## B-007 — NORA: "ERROR:unknown command" on AUDIO:FILE header
- **Symptom:** NORA rejects AUDIO:FILE header — line buffer had garbage from boot noise
- **Root Cause:** NORA's `pos` variable non-zero from UART glitches during STM32 reset; AUDIO:FILE matched at wrong offset
- **Fix:** STM32 sends `"\n\n"` before AUDIO:FILE header to flush NORA's line buffer to pos=0
- **Status:** FIXED

---

## B-008 — SD: Concurrent FatFS access corrupts win[] cache
- **Symptom:** FR_DISK_ERR mid-read during streaming
- **Root Cause:** HealthMonTask f_getfree running concurrently with AudioSD_SendFileToUART — both touching FatFS win[] sector cache
- **Fix:** `s_sdBusy` flag; HealthMonTask skips f_getfree while streaming
- **Status:** FIXED

---

## B-009 — SD: f_write PCM fails (code=7) after DFSDM→SDMMC transition
- **Symptom:** REC_ERR_SD_WRITE_PCM on first write after recording; f_open or f_write returns FR_DISK_ERR
- **Root Cause:** DFSDM DMA→SDMMC transition leaves SDMMC DPSM dirty (RX overrun 0x20); SDMMC not ready for FatFS read/write
- **Fix:** `AudioSD_Remount()` called in SDWriteTask before every `f_open`
- **Status:** FIXED

---

## B-010 — SD: f_getfree fails (fr=3) after write error
- **Symptom:** HealthMonTask reports `f_getfree fail: fr=3` (FR_NOT_READY) after a failed recording
- **Root Cause:** Error path in SDWriteTask left SDMMC dirty; HealthMonTask f_getfree hit the dirty state
- **Fix:** `if (logMsg.result != REC_OK) AudioSD_Remount()` at `done:` label in SDWriteTask
- **Status:** FIXED

---

## B-011 — SD: disk_read RXOVERR (0x20) during UART streaming — ICR-clear retry fails
- **Symptom:**
  ```
  [SD] disk_read FAIL sec=N sdErr=0x00000020 retrying
  [SD] disk_read retry FAIL sec=N sdErr=0x80000000
  ```
  NORA UART timeout. File transfer fails at varying byte offsets.
- **Root Cause:** AXI bus contention — LTDC DMA bursts starve SDMMC IDMA FIFO → RXOVERR (0x20).
  After RXOVERR the SDMMC DPSM (Data Path State Machine) is stuck.
  ICR-clear approach (`IDMACTRL=0, DCTRL=0, clear flags, force State=READY`) is insufficient:
  - After ICR-clear, `HAL_SD_GetCardState` (CMD13) returns `REQUEST_NOT_APPLICABLE (0x80000000)`
  - CMD13 fails because DPSM is still active — card thinks a data transfer is in progress
  - GetCardState 500ms timeout → RES_ERROR → FR_DISK_ERR
- **Mitigations applied:**
  - SDMMC ClockDiv 2→4 (50MHz→25MHz): doubles FIFO fill time, reduces RXOVERR frequency
  - Recording reduced 5s→3s: file size 160KB→96KB, fewer sectors, fewer RXOVERR events
- **Fix applied (2026-04-05):** Full `HAL_SD_DeInit + 20ms + SDMMC1_Peripheral_Init` inside
  `disk_read` retry path. After re-init, card is back in TRANSFER state (CMD0/CMD8/ACMD41/CMD7
  sequence completed). Cost: ~30ms per retry. NORA chunk timeout: 5000ms. Acceptable.
- **Status:** FIX APPLIED — awaiting test results

---

## B-012 — SD: HealthMonTask FR_NOT_READY on first f_getfree (no recording)
- **Symptom:** `[HEALTH] f_getfree fail: fr=3` at T+11407ms, no recording triggered
- **Root Cause:** `disk_status()` calls `HAL_SD_GetCardState()` (CMD13) every FatFS operation.
  After 11s idle, first CMD13 fails transiently (SDMMC CPSM in degraded state from boot-time
  reads during f_mount). FatFS gets STA_NOINIT → FR_NOT_READY.
  Recording pipeline unaffected (SDWriteTask always calls AudioSD_Remount before f_open).
- **Fix applied (2026-04-05):** HealthMonTask calls `AudioSD_Remount()` on FR_NOT_READY then
  retries f_getfree. Self-healing diagnostic.
- **Status:** FIXED — verified T+10258 and T+20259 both show `card=PRESENT` in next test

---

## B-013 — DMA: g_SampleCount stays 0 after recording (DMA semaphore never fires)
- **Symptom:** `g_SampleCount = 0` at BP03 (after DMA stop). `g_AudioBuf` contains stale data from previous session. LED does not blink during recording window.
- **Root Cause:** `xSemaphoreTake(s_dmaSem)` never returns `pdTRUE` during drain loop — meaning `HAL_DFSDM_FilterRegConvHalfCpltCallback` and `HAL_DFSDM_FilterRegConvCpltCallback` are not firing. DMA started (`HAL_DFSDM_FilterRegularStart_DMA` returned HAL_OK at BP02) but no interrupts generated. Possible causes: DMA IRQ not enabled, DFSDM clock not running, or DMAMUX misconfigured.
- **Discovered:** 2026-04-05 during IAR debug session, confirmed by Watch window at BP03
- **Workaround:** If `g_SampleCount == 0` after drain loop, fill `g_AudioBuf` with 1 kHz sine test tone (16-sample table, amplitude 8000) so pipeline can be tested end-to-end.
- **Status:** OPEN — workaround in place, root cause (DMA IRQ/clock) not yet diagnosed

---

## Open / Pending

### RXOVERR during AudioSD_Remount f_mount (B-011 variant)
- **Symptom:**
  ```
  [SD] disk_read FAIL sec=0 sdErr=0x00000020 — reinit+retry
  [SD] disk_read retry FAIL sec=0 sdErr=0x80000000
  ```
  RXOVERR hitting sector 0 (MBR) read inside `f_mount(opt=1)` called by AudioSD_Remount.
  If retry also fails → f_mount returns FR_DISK_ERR → s_sdReady=false → f_open fails.
- **Last seen:** T+24708ms (2026-04-05 test)
- **Status:** B-011 fix (DeInit+Init in disk_read retry) should also cover this path. Awaiting test.

---

## Pipeline Timing (nominal, 3-second recording)

| Stage | Duration |
|-------|----------|
| A: Record (3s DMA drain) | ~3050 ms |
| B.0: Remount + WAV write (96 KB) | ~200 ms |
| B.1: NORA TLS handshake | ~620 ms |
| B.2: UART stream (96 KB @ 921600 baud) | ~1040 ms |
| B.4: GCS upload + STT | ~2000 ms |
| **Total pipeline** | **~7 s nominal** |
| NORA per-chunk timeout | 5000 ms |
