# Debug Macro — Voice Record Pipeline

## Macro file path

```
C:\TouchGFXProjects\MyApplication\EWARM\debug_breakpoints.mac
```

## How to load in IAR

### Option A — load once per session
1. Open IAR: `EWARM\STM32H747I-DISCO.eww`
2. `Debug → Macros → Load Macro...`
3. Select: `EWARM\debug_breakpoints.mac`
4. Press **Download and Debug** (Ctrl+D)
5. All 6 breakpoints appear automatically

### Option B — load automatically every session
1. `Project → Options → Debugger → Setup`
2. Check **"Use macro file"**
3. Browse to: `EWARM\debug_breakpoints.mac`
4. Click OK — breakpoints are set on every debug session from now on

---

## Breakpoints set by the macro (26 hardware breakpoints)

### A — VoiceRecTask
| BP | File | Line | Description |
|----|------|------|-------------|
| BP01 | voice_recorder.c | 221 | VoiceRecTask woke (button pressed) |
| BP02 | voice_recorder.c | 232 | DMA recording started |
| BP03 | voice_recorder.c | 270 | 3 s done — DMA stopped |
| BP04 | voice_recorder.c | 280 | xQueueSend → signal SDWriteTask |

### B — SDWriteTask write phase
| BP | File | Line | Description |
|----|------|------|-------------|
| BP05 | voice_recorder.c | 340 | AudioSD_Remount before f_open |
| BP06 | voice_recorder.c | 342 | f_open WAV file for writing |
| BP07 | voice_recorder.c | 353 | f_write WAV header |
| BP08 | voice_recorder.c | 376 | f_write PCM chunk loop |
| BP09 | voice_recorder.c | 389 | f_close WAV file |
| BP10 | voice_recorder.c | 396 | WAV written OK |

### C — SDWriteTask stream phase
| BP | File | Line | Description |
|----|------|------|-------------|
| BP11 | voice_recorder.c | 414 | AudioSD_SendFileToUART attempt |
| BP12 | voice_recorder.c | 420 | All 3 send attempts failed |
| BP13 | voice_recorder.c | 431 | xQueueSend → RTTLogTask |

### D — AudioSD_SendFileToUART internals
| BP | File | Line | Description |
|----|------|------|-------------|
| BP14 | audio_sd.c | 332 | Entry Remount |
| BP15 | audio_sd.c | 336 | f_stat (get file size) |
| BP16 | audio_sd.c | 362 | Waiting 2s for NORA TLS |
| BP17 | audio_sd.c | 369 | f_open for reading (after TLS delay) |
| BP18 | audio_sd.c | 385 | f_read chunk loop |
| BP19 | audio_sd.c | 388 | FREAD_ERR — f_read failed |
| BP20 | audio_sd.c | 399 | UART_TX_ERR |
| BP21 | audio_sd.c | 405 | Stream done log |
| BP22 | audio_sd.c | 407 | f_close after streaming |
| BP23 | audio_sd.c | 412 | Exit Remount |

### E — disk_read error path **(the bug)**
| BP | File | Line | Description |
|----|------|------|-------------|
| BP24 | audio_sd.c | 629 | disk_read FAIL — first failure |
| BP25 | audio_sd.c | 639 | disk_read retry FAIL |

### F — AudioSD_Remount
| BP | File | Line | Description |
|----|------|------|-------------|
| BP26 | audio_sd.c | 461 | Remount entry |

---

## What to inspect at BP24 (the bug)

Open the **Watch** window and add:

| Expression | Expected | Meaning |
|------------|----------|---------|
| `s_hsd1.ErrorCode` | `0x00000020` = RXOVERR | SDMMC FIFO overrun |
| `s_hsd1.State` | `HAL_SD_STATE_ERROR` | HAL stuck |
| `SDMMC1->STA` | check bits | live peripheral status |
| `sector` | ~88 | sector number where failure hits |
