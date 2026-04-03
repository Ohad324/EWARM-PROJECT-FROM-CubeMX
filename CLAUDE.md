# CLAUDE.md — STM32H747I-DISCO Voice Recorder (v9)
M7 Core | CubeMX + IAR | DFSDM + FatFS + FreeRTOS

## Quick Reference
- **Board**: STM32H747I-DISCO, Cortex-M7, CubeMX + IAR
- **IAR project**: `EWARM/STM32H747I-DISCO.ewp` — active target: `STM32H747I-DISCO_CM7`
- **Mic**: MP34DT01 MEMS onboard, DFSDM1 Filter 0, 16 kHz 16-bit mono
- **Trigger**: Blue button PC13 (Phase 1) / Hey Nora wake word (Phase 2)
- **Record**: 5-second fixed window, audio stored in SDRAM g_AudioBuf
- **Save**: SDWriteTask writes REC_001.wav to SD card via FatFS/SDMMC1
- **Log**: RTTLogTask prints filename + result code to Segger RTT ch0
- **PC view**: USB-MSC on CN1 (second cable) — NORA_WavViewer.html
- **USB power**: CN2 (already connected) — powers board + ST-LINK debug

## Mandatory Rules

1. NEVER use `vTaskDelay` for the 5-second timer — use active DMA drain loop
2. ALWAYS declare `g_State` as `volatile` + `__DSB()` after every write
3. ALWAYS freeze `g_SampleCount` into local var at start of SDWriteTask
4. NEVER do `memcpy` in DMA ISR callbacks — post semaphore only
5. NEVER increment `g_FileIndex` on `f_open()` failure
6. ALWAYS check `g_SysMode` before touching SD card in any task
7. RTTLogTask priority = 1 (NOT 2 — must be below TzCtrl)
8. `xLogQueue` send timeout = `pdMS_TO_TICKS(100)`, NOT 0

## Think Before You Act

Before writing any code or making any change:
1. **Read the relevant code** — never diagnose from memory alone
2. **Identify the root cause** — trace the exact failure path
3. **List ALL options** — at least 2–3 alternatives with trade-offs
4. **Present the analysis** — explain what you found and why before proposing anything
5. **Get confirmation** — agree on the approach before touching a single file

Do NOT jump to writing code the moment a problem is described.
If the first solution fails, do NOT immediately try another — go back to step 1.

## Work Log
Document every step in `doc/worklog.md`. Read it at the start of each session.
Update every 10 minutes with a timestamp and what was done/changed.
Commit all changes every 10 minutes with a short descriptive message.

## Debugging Rules

### Always use HARDWARE breakpoints — never software
In all C-SPY `.mac` files use `__setHWCodeBreak()` instead of `__setCodeBreak()`.
Software breakpoints patch flash with BKPT instructions — they can corrupt the image
and cause false halts in ISR/HAL code. The Cortex-M7 FPB unit has 8 HW BP registers.

## Exit Codes — REC_Result_t

```c
typedef enum {
    REC_OK               = 0,
    REC_ERR_DMA_START    = 1,   /* HAL_DFSDM_FilterRegularStart_DMA failed */
    REC_ERR_DMA_OVERFLOW = 2,   /* g_AudioBuf full before 5 s elapsed */
    REC_ERR_DMA_STOP     = 3,   /* HAL_DFSDM_FilterRegularStop_DMA failed */
    REC_ERR_SD_MOUNT     = 4,   /* f_mount returned non-FR_OK */
    REC_ERR_SD_OPEN      = 5,   /* f_open failed — card missing or full */
    REC_ERR_SD_WRITE_HDR = 6,   /* f_write WAV header failed */
    REC_ERR_SD_WRITE_PCM = 7,   /* f_write PCM data failed — partial write */
    REC_ERR_SD_CLOSE     = 8,   /* f_close failed */
    REC_ERR_QUEUE_FULL   = 9,   /* xLogQueue full — log message dropped */
} REC_Result_t;
```

## Microphone Architecture — One Mic, Two Roles

The MP34DT01 connects to DFSDM1 Filter 0, which has exactly one DMA channel:

| | Phase 1 — button | Phase 2 — Hey Nora |
|---|---|---|
| DMA runs when? | Only during 5-second recording | Always — listening + recording |
| DMA buffer | g_DmaBuf (512 × int32) in D2 SRAM | g_WwBuf (ring) for wake word, then g_DmaBuf |
| DMA owner | VoiceRecTask only | WakeWordTask → hands to VoiceRecTask on detection |
| Trigger source | Blue button PC13 EXTI ISR | WakeWordTask detects keyword → xTaskNotify() |

## Complete FreeRTOS Task Map

| Task name | Prio | Stack | Phase | Role |
|-----------|------|-------|-------|------|
| TouchGFXTask | 7 | 4096 w | 1+2 | TouchGFX display rendering |
| UARTReceiveTask | 6 | 512 w | 1+2 | BLE/NORA UART bridge via DMA |
| VoiceRecTask | 6 | 512 w | 1+2 | Wakes on trigger, runs 5s timer, copies DMA chunks int32→int16 into SDRAM, signals SDWriteTask |
| CmdHandler | 5 | 512 w | 1+2 | Routes UART commands |
| videoTask | 4 | 512 w | 1+2 | Video decode and playback |
| music_disp | 3 | 2048 w | 1+2 | Music display and audio output |
| SDWriteTask | 3 | 512 w | 1+2 | f_open/f_write/f_close → REC_xxx.wav |
| TzCtrl | 2 | 512 w | 1+2 | Percepio Tracealyzer streaming |
| RTTLogTask | 1 | 256 w | 1+2 | Prints result to RTT ch0 after SDWriteTask posts to xLogQueue |
| WakeWordTask | 1 | 1024 w | 2 | Phase 2 only — keyword detection, notifies VoiceRecTask |
| IDLE | 0 | 128 w | 1+2 | FreeRTOS system idle task |

## Complete Memory Map

| Memory region | Size | Owner | Contents |
|---|---|---|---|
| Flash (2 MB) | 2 MB | none | Firmware code only — audio never touches Flash |
| D2 SRAM — g_DmaBuf | 2 KB | VoiceRecTask | DFSDM DMA ping-pong buffer. 512 × int32. Must stay in D2 SRAM — DMA cannot access SDRAM directly. |
| D2 SRAM — g_WwBuf | 8 KB | WakeWordTask | Phase 2 only. Circular ring buffer for keyword detection (~1 s of audio). |
| D1 SRAM | varies | FreeRTOS | Task stacks, TCBs, OS heap. No audio data. |
| SDRAM — g_AudioBuf | 160 KB | VoiceRecTask+SDWriteTask | 5s × 16000 Hz × 2 bytes = 160 KB. Address 0xD0000000. Needs `__attribute__((section(".sdram")))` and .icf entry. |
| SD card (FatFS) | ~320 KB | SDWriteTask | WAV header (44 bytes) + PCM data (160 KB). Auto-increments: REC_001.wav, REC_002.wav… |

### Memory Flow
```
Mic → DFSDM DMA → D2 SRAM g_DmaBuf (2KB ping-pong)
     → CPU copy int32>>8→int16 → SDRAM g_AudioBuf (160KB, 0xD0000000)
     → SDWriteTask f_write → SD card REC_001.wav
     → RTTLogTask SEGGER_RTT_printf → RTT Viewer
Flash is NEVER written.
```

## IAR Linker Script (.icf)
```
define region SDRAM_region = mem:[from 0xD0000000 size 0x2000000];
place in SDRAM_region { section .sdram };
```

## Shared Structs

```c
typedef struct {
    char         filename[32];
    uint32_t     sizeBytes;
    uint8_t      success;
    REC_Result_t result;     /* exact stage that failed */
    uint32_t     timestamp;  /* HAL_GetTick() at time of save */
} LogMsg_t;

QueueHandle_t xLogQueue;  /* global, depth 4, sizeof(LogMsg_t) */
```

## SDWriteTask — Exact Sequence

```c
void SDWriteTask(void *arg) {
    QueueHandle_t q = (QueueHandle_t)arg;
    uint32_t msg;
    for (;;) {
        if (g_SysMode != SYS_MODE_RECORD) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        xQueueReceive(q, &msg, portMAX_DELAY);

        g_State = REC_SAVING;
        __DSB();
        uint32_t samplesSnapshot = g_SampleCount;   /* freeze immediately */

        LogMsg_t logMsg = {0};
        logMsg.timestamp = HAL_GetTick();
        logMsg.sizeBytes  = samplesSnapshot * sizeof(int16_t);
        logMsg.result     = REC_OK;

        char filename[32];
        snprintf(filename, sizeof(filename), "REC_%03d.wav", g_FileIndex + 1);
        strncpy(logMsg.filename, filename, sizeof(logMsg.filename));

        FIL file; UINT bw; WavHeader hdr;
        FRESULT fr = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr != FR_OK) { logMsg.result = REC_ERR_SD_OPEN; goto done; }

        g_FileIndex++;   /* only after successful open */
        BuildWavHeader(&hdr, samplesSnapshot);

        fr = f_write(&file, &hdr, sizeof(hdr), &bw);
        if (fr != FR_OK || bw != sizeof(hdr))
            { logMsg.result = REC_ERR_SD_WRITE_HDR; f_close(&file); goto done; }

        fr = f_write(&file, g_AudioBuf, logMsg.sizeBytes, &bw);
        if (fr != FR_OK || bw != logMsg.sizeBytes)
            { logMsg.result = REC_ERR_SD_WRITE_PCM; f_close(&file); goto done; }

        if (f_close(&file) != FR_OK) { logMsg.result = REC_ERR_SD_CLOSE; goto done; }
        logMsg.success = 1;

done:
        if (xQueueSend(xLogQueue, &logMsg, pdMS_TO_TICKS(100)) != pdTRUE)
            SEGGER_RTT_printf(0, "[REC] xLogQueue full — log dropped\r\n");
        g_State = REC_IDLE;
        __DSB();
    }
}
```

## RTTLogTask

```c
void RTTLogTask(void *arg) {
    /* Print bug log at every boot */
    SEGGER_RTT_printf(0, "\r\n=== BUG LOG ===\r\n");
    SEGGER_RTT_printf(0, "B-001 BUILD  .sdram section missing         FIXED\r\n");
    SEGGER_RTT_printf(0, "B-002 DMA    vTaskDelay drain bug           FIXED\r\n");
    SEGGER_RTT_printf(0, "B-003 SD     f_mount called too late        FIXED\r\n");
    SEGGER_RTT_printf(0, "=== END BUG LOG ===\r\n\r\n");

    LogMsg_t msg;
    for (;;) {
        xQueueReceive(xLogQueue, &msg, portMAX_DELAY);
        if (msg.result == REC_OK) {
            SEGGER_RTT_printf(0, "[REC] OK      %s  %lu bytes  t=%lu ms\r\n",
                msg.filename, (unsigned long)msg.sizeBytes, (unsigned long)msg.timestamp);
        } else {
            SEGGER_RTT_printf(0, "[REC] FAIL    %s  code=%d  t=%lu ms\r\n",
                msg.filename, (int)msg.result, (unsigned long)msg.timestamp);
        }
    }
}
```

## VoiceRecTask — Active DMA Drain Loop (CRITICAL)

```c
/* WRONG — DO NOT USE */
VoiceRec_Start();
vTaskDelay(pdMS_TO_TICKS(5000));   /* nobody drains DMA during sleep */
VoiceRec_Stop();

/* CORRECT — active drain during 5-second window */
void VoiceRecTask(void *arg) {
    QueueHandle_t q = (QueueHandle_t)arg;
    uint32_t notif;
    for (;;) {
        xTaskNotifyWait(0, ULONG_MAX, &notif, portMAX_DELAY);

        HAL_StatusTypeDef hal = HAL_DFSDM_FilterRegularStart_DMA(...);
        if (hal != HAL_OK) {
            LogMsg_t err = {0};
            err.result = REC_ERR_DMA_START;
            err.timestamp = HAL_GetTick();
            strncpy(err.filename, "NONE", sizeof(err.filename));
            xQueueSend(xLogQueue, &err, pdMS_TO_TICKS(100));
            g_State = REC_IDLE; __DSB();
            continue;
        }

        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        while (xTaskGetTickCount() < deadline) {
            if (xSemaphoreTake(g_DmaSem, pdMS_TO_TICKS(10)) == pdTRUE)
                StoreDmaChunk();
            if (g_SampleCount >= AUDIO_BUFFER_SAMPLES) break;
        }
        HAL_DFSDM_FilterRegularStop_DMA(...);

        uint32_t msg = 1;
        xQueueSend(q, &msg, 0);
    }
}
```

### DMA ISR Callbacks — Post Semaphore Only
```c
/* HAL_DFSDM_FilterRegConvHalfCpltCallback and CpltCallback — only this, nothing else */
xSemaphoreGiveFromISR(g_DmaSem, &xHigherPriorityTaskWoken);
portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
```

## Phase 1 — Button ISR

```c
#define DEBOUNCE_MS  300

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin != GPIO_PIN_13) return;
    if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) != GPIO_PIN_RESET) return;
    static uint32_t lastPress = 0;
    uint32_t now = HAL_GetTick();
    if ((now - lastPress) < DEBOUNCE_MS) return;
    lastPress = now;

    if (g_SysMode == SYS_MODE_RECORD) {
        if (VoiceRec_GetState() == REC_IDLE)
            xTaskNotifyFromISR(voiceRecTaskHandle, 1, eSetBits, NULL);
    } else {
        /* Toggle back to RECORD mode */
        USBD_Stop(&hUsbDeviceHS);
        f_mount(&SDFatFS, "", 1);
        g_SysMode = SYS_MODE_RECORD; __DSB();
        SEGGER_RTT_printf(0, "[MODE] RECORD mode active\r\n");
    }
}
```

## main.c — Task Creation

```c
#include "voice_recorder.h"

TaskHandle_t  voiceRecTaskHandle = NULL;   /* global — used by ISR */
QueueHandle_t xVoiceQueue;
QueueHandle_t xLogQueue;

/* After all MX_xxx_Init() and f_mount(): */
VoiceRec_Init();
xVoiceQueue = xQueueCreate(1, sizeof(uint32_t));
xLogQueue   = xQueueCreate(4, sizeof(LogMsg_t));

xTaskCreate(VoiceRecTask, "VoiceRecTask", 512,  xVoiceQueue, 6, &voiceRecTaskHandle);
xTaskCreate(SDWriteTask,  "SDWriteTask",  512,  xVoiceQueue, 3, NULL);
xTaskCreate(RTTLogTask,   "RTTLogTask",   256,  NULL,        1, NULL);
/* Phase 2: xTaskCreate(WakeWordTask, "WakeWordTask", 1024, NULL, 1, NULL); */
```

## USB-MSC — Reading WAV Files on PC

### USB Cable Setup

| Connector | Type | Purpose | Action required |
|---|---|---|---|
| CN2 | Micro-B | ST-LINK debug | ALREADY CONNECTED — keep. Programs board, powers it, RTT, IAR debug. |
| CN1 | Micro-AB | USB OTG HS (MSC) | ADD A SECOND CABLE HERE — Windows sees it as a removable drive. |
| CN14 | Micro-B | 5V power only | DO NOT USE for data. |

Two cables plugged into PC simultaneously: CN2 for debug, CN1 for the WAV drive.

### CubeMX Config for USB-MSC on CN1
- Enable USB_OTG_HS → Mode: Device Only
- Enable USB_DEVICE middleware → Class: Mass Storage Class (MSC)
- `STORAGE_Read_FS` / `STORAGE_Write_FS` map to existing SDMMC1 FatFS driver

### SysMode_t — SD Card Ownership

```c
typedef enum {
    SYS_MODE_RECORD  = 0,   /* FatFS owned by STM32 — recording active */
    SYS_MODE_USB_MSC = 1,   /* FatFS handed to USB-MSC — PC can read */
} SysMode_t;
volatile SysMode_t g_SysMode = SYS_MODE_RECORD;
__DSB();
```

**Switch to USB-MSC mode (in button ISR, when already in RECORD mode):**
```c
if (VoiceRec_GetState() == REC_IDLE) {
    f_mount(NULL, "", 0);
    USBD_Start(&hUsbDeviceHS);
    g_SysMode = SYS_MODE_USB_MSC; __DSB();
    SEGGER_RTT_printf(0, "[MODE] USB-MSC active — PC can read SD\r\n");
}
```

## Phase 2 — WakeWordTask (future)

```c
void WakeWordTask(void *arg) {
    HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0, g_WwBuf, WW_BUF_SIZE);
    for (;;) {
        if (KeywordDetector_Feed(g_WwBuf, WW_BUF_SIZE) == KW_DETECTED) {
            HAL_DFSDM_FilterRegularStop_DMA(&hdfsdm1_filter0);
            xTaskNotify(voiceRecTaskHandle, 1, eSetBits);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            HAL_DFSDM_FilterRegularStart_DMA(&hdfsdm1_filter0, g_WwBuf, WW_BUF_SIZE);
        }
        vTaskDelay(1);
    }
}
```

WakeWordTask MUST call `HAL_DFSDM_FilterRegularStop_DMA()` before notifying VoiceRecTask —
both cannot own the DMA simultaneously.

## LED Feedback

| State | LED (PI12) | Meaning |
|---|---|---|
| RECORD mode — IDLE | Off | Ready to record |
| RECORD mode — recording | Blinks 500 ms | 5-second window active |
| RECORD mode — saving | Solid ON | SDWriteTask writing — wait |
| USB-MSC mode | Slow blink 1 s | PC has SD card — do not record |

## Percepio TraceRecorder Integration (CM7 only)

### SDK location
`Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/` — v4.11.1, streaming-only API.

### Files added to IAR project (CM7 build only, excluded from CM4)
- All `trc*.c` from `Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/` root
- `Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/streamports/Jlink_RTT/trcStreamPort.c`
- `Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/streamports/Jlink_RTT/SEGGER_RTT.c`

### Include paths added (CM7 only)
- `Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/include`
- `Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/config`
- `Percepio31-3/Tracealyzer 4/FreeRTOS/TraceRecorder/streamports/Jlink_RTT/include`

### SEGGER_RTT version conflict — resolved
Old `CM7/Core/Src/SEGGER_RTT.c` is **excluded from CM7 build** — replaced by the Percepio
version. `CM7/Core/Inc/SEGGER_RTT.h` and `SEGGER_RTT_Conf.h` replaced with Percepio v7.96o.

### v4.11.1 API — settings that do NOT exist (do not add)
- `TRC_CFG_RECORDER_MODE` — streaming is the only mode
- `TRC_CFG_STREAM_PORT` — port selected by which .c file is compiled, not a define
- `TRC_CFG_NTASK`, `TRC_CFG_NQUEUE`, `TRC_CFG_NSEMAPHORE`, `TRC_CFG_NMUTEX`, `TRC_CFG_NTIMER` — removed in v4.8+

### Configuration state
- `TRC_CFG_HARDWARE_PORT` → `TRC_HARDWARE_PORT_ARM_Cortex_M` ✅ done
- Processor header → `#include "stm32h7xx.h"` ✅ done
- `TRC_CFG_STREAM_PORT_RTT_UP_BUFFER_SIZE` → `(1024 * 16)` — pending
- `TRC_CFG_STREAM_PORT_RTT_DOWN_BUFFER_SIZE` → `(1024 * 2)` — pending
- `SEGGER_RTT_Conf.h` BUFFER_SIZE_UP → `(1024 * 16)`, BUFFER_SIZE_DOWN → `(1024 * 2)` — pending
- `configUSE_STATS_FORMATTING_FUNCTIONS` → add `1` — pending
- ⚠️ CONFLICT: `FreeRTOSConfig.h` defines custom `traceXXX` macros for rtos_trace.c.
  `trcKernelPort.h` redefines same macros — will cause redefinition errors. Must choose:
  - Option A: Remove custom traceXXX macros (rtos_trace.c loses kernel hooks)
  - Option B: Keep both with `#undef` guards (both systems coexist)
- `main.c`: add `#include "trcRecorder.h"` + `xTraceEnable(TRC_START)` before `osKernelStart()` — pending
- CM4 must NOT have `configUSE_TRACE_FACILITY 1` — confirm before each build

## Key Files

| File | Purpose |
|------|---------|
| `CM7/Core/Src/main.c` | UARTReceiveTask, THUMB state machine, task creation |
| `CM7/Core/Src/music_display_task.c` | JpegDisplayTask, JPEG decode, xMusicQueue |
| `CM7/Core/Src/ble_uart.c` | DMA+IDLE receive, 128-byte bursts → xRawBleQueue |
| `CM7/Core/Src/audio_sd.c` | SD card WAV recording |
| `CM7/Core/Src/command_handler.c` | CMD: message routing |
| `tools/youtube_player.py` | PC HTTP server — receives NORA POSTs, opens Chrome |
| `NORA_BLE/nora_wifi_ble_translate/main/nora_ble_bridge.c` | WiFi credentials at lines 57–58 |
| `NORA_BLE/nora_wifi_ble_translate/main/music_task.c` | YouTube search, UART send to STM32 |

## Voice Command Pipeline — New Feature (`feature/voice-record` branch)

This feature is developed on a separate branch to avoid breaking the working thumbnail pipeline.
Do NOT merge into master until each stage is tested end-to-end.

### Stage 1 — Record to SD card (STM32 side)
File: `CM7/Core/Src/audio_sd.c` / `audio_sd.h`

- Button press → open `REC_001.wav` on SD card (auto-incrementing index)
- Write placeholder 44-byte WAV header at start
- As each DMA chunk arrives (via semaphore drain loop), append PCM frames using FatFS
- On stop: seek to byte 0, overwrite WAV header with correct file size and data length
- Close file, then send `AUDIO:FILE:REC_001.wav:<size>\n` over UART8 to NORA
- Audio format: **16666 Hz, 16-bit, mono** (actual DFSDM rate — use this in WAV header)
- Tell Google STT **16000 Hz** in the API request (closest valid rate; STT resamples)
- Do NOT call `Error_Handler()` — log `SD:INIT_FAIL` and return if card absent

### Stage 2 — Upload to Google Cloud Storage (NORA/ESP32 side)
File: `NORA_BLE/nora_wifi_ble_translate/main/cloud_upload.c` / `cloud_upload.h`

- On receiving `AUDIO:FILE:filename.wav:<size>\n`, upload WAV to GCS bucket via HTTPS
- Bucket name and credentials stored as `#define` constants at top of file
- On result, send back to STM32: `UPLOAD:OK:filename.wav\n` or `UPLOAD:FAIL:filename.wav\n`

### Stage 3 — Speech-to-Text transcription (NORA/ESP32 side)
File: `cloud_upload.c` (same module)

- Immediately after upload, call Google Cloud Speech-to-Text API on the uploaded file
- Audio format passed explicitly in API request: **16000 Hz**, 16-bit, mono, LINEAR16
  - Actual DFSDM output rate ≈ 16,666 Hz (100 MHz / (2×24) / 125), but pass 16000 to STT API
  - Google STT resamples internally — 16000 is the closest valid rate and is accepted
- API key stored as `#define SPEECH_API_KEY ""`
- Result: plain-text transcript e.g. `"play Beatles"` or `"turn off the lights"`

### Stage 4 — Command routing (NORA/ESP32 side)
File: `NORA_BLE/nora_wifi_ble_translate/main/command_router.c` / `command_router.h`

Parse the transcript and route using these rules:

**Rule A — Media commands** (`play`, `stop`, `pause`, `next`, `previous`, `volume`):
- POST `{"command": "play Beatles"}` to PC at `http://PC_IP:PORT/command`
- `PC_IP` and `PORT` stored as `#define` constants
- PC server (`tools/youtube_player.py`) handles via `_handle_voice_command()`

**Rule B — Screen/display commands** (`show`, `display`, `screen`, `clear`, `update`):
- Send `CMD:<transcript>\n` to STM32 over UART8
- STM32 `CommandHandler` task parses and updates the display

**Rule C — Unknown:**
- Send `CMD:UNKNOWN\n` to STM32, log unrecognised transcript over debug serial

### Constraints
- Do NOT break existing UART streaming, DMA double-buffer, or button debounce logic
- Handle errors at every stage: SD failure, WiFi drop, upload failure, API timeout, unknown command
- Each new function commented in the same style as existing code
- `AUDIO:FILE:` message from STM32 → NORA must include byte size so NORA knows when transfer ends
- NORA must NOT block the STM32 pipeline waiting for upload/STT results

---

## Build & Flash
1. **ESP32**: `idf.py build flash monitor` from `NORA_BLE/nora_wifi_ble_translate/`
2. **STM32**: IAR → Build → Download and Debug (CM7 target)
3. **PC**: `python tools/youtube_player.py` (must be running before playing)

## IAR Tool Paths
All IAR build and debug utilities are located under `C:\TouchGFXProjects\IAR 9.70.2\`:
- **iarbuild.exe**: `C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\iarbuild.exe`
- **cspybat.exe**: `C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\cspybat.exe`
- NOT under `C:\iar\` or `C:\Program Files\IAR Systems\`

**Build command (CM7):**
```
"C:\TouchGFXProjects\IAR 9.70.2\ewarm-9.70.2\common\bin\iarbuild.exe" STM32H747I-DISCO.ewp -build STM32H747I-DISCO_CM7
```
Run from `EWARM\` directory.

**Before building STM32:** check `EWARM/STM32H747I-DISCO.ewp` — `thumb_pipeline.c` and
`thumb_ref_jpeg.c` were deleted from disk but may still be listed. Remove them if present.

## Start NORA Player — desktop bat file
`C:\Users\Ohad\Desktop\Start NORA Player.bat`

**Tool paths:**
- cspybat: `C:\iar\ewarm-9.70.2\common\bin\cspybat`
- RTT Viewer: `C:\Program Files\SEGGER\JLink_V930a\JLinkRTTViewer.exe`

## Living Bug Log

| ID | Time | Stage | Symptom | Root cause | Fix applied | Status |
|----|------|-------|---------|------------|-------------|--------|
| B-001 | t=0 ms | BUILD | .sdram section not found by linker | section .sdram missing in .icf file | Added region to .icf | FIXED |
| B-002 | t=? ms | DMA | No audio chunks arriving in RTT | vTaskDelay used instead of active drain loop | Replaced with semaphore drain loop | FIXED |
| B-003 | t=? ms | SD | REC_ERR_SD_OPEN in RTT log | SD card not mounted before task start | Moved f_mount before xTaskCreate | FIXED |
| B-004 | | | EXTI | LED not toggling on button press | IT_FALLING used instead of IT_RISING — board pulls PC13 HIGH on press (10k pull-down to GND) | Changed to GPIO_MODE_IT_RISING in Button_GPIO_Init() | FIXED |

**Stage codes:** BUILD, INIT, DMA, DRAIN, SD, RTT, ISR, STATE, RTOS, PHASE2, OTHER
**Status values:** OPEN, WIP, FIXED, WONTFIX

## Git Auto-Commit — Bug Log Policy

Every time the bug log changes, commit immediately:
```bash
git add CLAUDE.md
git commit -m "buglog: <action> <bug-id> | open=<N> fixed=<N> wip=<N> | $(date '+%Y-%m-%d %H:%M')"
```
Example: `buglog: FIXED B-004 semaphore drain loop | open=0 fixed=4 wip=0 | 2026-04-02 15:01`

---

## Music Display Pipeline (existing system)
iPhone → BLE "PLAY:song" → NORA (ESP32) → PC scrape → UART8 → STM32H747 → LCD thumbnail + title
STM32 mic → NORA → GCS upload → Speech-to-Text → "play song" → YouTube Data API → UART8 → STM32

### Dual-Path Music Architecture (IMPORTANT — both paths must stay working)

Two independent trigger sources, each with its own search method:

| Source | Trigger | Search method | API key needed? | PC opens YouTube? |
|--------|---------|---------------|-----------------|-------------------|
| **BLE** | iPhone → NRF → `music_request()` | `search_via_pc()` — PC scrapes YouTube | No | Yes (PC does it in search_via_pc) |
| **Voice** | STM32 mic → GCS STT → `music_request_voice()` | `youtube_search()` — YouTube Data API v3 | Yes (enable in GCP Console) | Yes (post_to_pc_player after TRACK/THUMB) |

**Do NOT merge these paths.** They must remain independent so one can be tested/fixed without breaking the other.

#### BLE path (working)
```
iPhone → NRF BLE → nora_ble_bridge.c detects "PLAY:" → music_request(query)
  → music_task: source=BLE → search_via_pc()
     POST {"query":"metallica"} to PC /play
     PC scrapes YouTube, opens Chrome, downloads thumbnail, responds {"videoId":"xxx"}
  → NORA fetches thumbnail from PC /thumbnail
  → UART: TRACK:title|channel|videoId\n + THUMB:<size>\n + JPEG bytes
```

#### Voice path (requires YouTube Data API v3 enabled in GCP Console)
```
STM32 mic → DFSDM → g_AudioBuf → AUDIO:FILE: to NORA
  → CloudUpload_UploadWav() → GCS bucket
  → CloudUpload_Transcribe() → Google Speech-to-Text → "play metallica"
  → CommandRouter_Route("play metallica")
     ContainsKeyword("play") → music_request_voice("play metallica")
  → music_task: source=VOICE → youtube_search()
     YouTube Data API v3 → videoId, title, thumbnail URL
  → UART: TRACK:title|channel|videoId\n
  → NORA fetches thumbnail from YouTube CDN (s_url_buf)
  → UART: THUMB:<size>\n + JPEG bytes
  → post_to_pc_player(videoId) → PC opens Chrome (fire-and-forget)
```

#### Playback controls (stop/pause/next/previous/volume) — voice only
```
CommandRouter_Route("stop") → RouteToPC() → POST {"command":"stop"} to PC /command
PC handles via _handle_voice_command() (media key presses via pyautogui)
```

### UART Protocol (NORA → STM32 over UART8, 921600 baud)
```
TRACK:<title>|<artist>|<videoId>\n
THUMB:<size>\n + binary JPEG bytes
ERROR:<reason>\n
RESULT:<word>\n   — Hebrew translation (existing pipeline)
CTRL:<action>\n   — Phase 2: play/pause/next/prev (stubbed)
```

### NORA must NOT wait for PC acknowledgment before streaming to STM32
The PC player is for playback only — it must never block the STM32 pipeline.
Mandatory execution order in music_task.c:
1. `youtube_search()` OR `search_via_pc()` — resolve videoId + thumbnail URL
2. Take UART mutex → send TRACK to STM32 → stream THUMB to STM32 → release mutex
3. `post_to_pc_player()` — notify PC AFTER STM32 is done (BLE path: PC already notified in step 1)

### Why the SD card init was crashing
`AudioSD_Init()` called `SDMMC1_Peripheral_Init()` before checking card presence.
If no SD card inserted, `HAL_SD_Init()` returned `HAL_ERROR` → `Error_Handler()` →
disabled all interrupts and looped forever, before the card-detect check (PI8).

**Fix applied (audio_sd.c):**
1. `SDMMC1_GPIO_Init()` first (so SD_DETECT pin PI8 is readable)
2. Check SD_DETECT — if card absent, return false immediately
3. Only then call `SDMMC1_Peripheral_Init()`
4. Made `HAL_SD_Init()` failure non-fatal (logs `SD:INIT_FAIL`, no crash)

**Rule:** Never call `Error_Handler()` inside peripheral init functions called unconditionally
from `main()`. Always check hardware presence first.

### Why BLE_UART_StartDMA was crashing (ORE / DMA BUSY)

**Symptom:** System boots, UART task starts, hits `Error_Handler` immediately.
IAR Watch shows `huart8.ErrorCode = 8 (ORE)`, `gState = READY`, `RxState = READY`.

**Root cause:** NORA sends UART data continuously during STM32 boot. Before
`BLE_UART_StartDMA()` arms the DMA, bytes arrive on UART8, overflow the FIFO →
ORE (0x08) is set. The ORE leaves the DMA handle in a non-READY state.
`HAL_UARTEx_ReceiveToIdle_DMA` finds DMA not READY → returns HAL_BUSY → HAL_ERROR.

**Fix applied (ble_uart.c):**
```c
__HAL_UART_CLEAR_FLAG(&huart8, UART_CLEAR_OREF | UART_CLEAR_FEF | ...);
huart8.ErrorCode = HAL_UART_ERROR_NONE;
huart8.RxState   = HAL_UART_STATE_READY;
HAL_DMA_Abort(huart8.hdmarx);   /* resets DMA handle State to READY */
```
Never call `Error_Handler()` from `BLE_UART_StartDMA`. NORA will always send data before
STM32 is ready — this is expected and must be handled gracefully.

### AudioRec is DISABLED — do not re-enable
`AudioRec_TaskEntry` must remain commented out in `main.c`.
DFSDM/GPIO conflict causes a crash before the thumbnail pipeline can display anything.
Only re-enable after the thumbnail pipeline is confirmed stable.

### Python script — always restart after changes
```
powershell -Command "Get-Process python,pythonw -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1; Start-Process python -ArgumentList 'C:\TouchGFXProjects\MyApplication\tools\youtube_player.py' -WindowStyle Normal"
```
Then verify: `netstat -ano | grep :5000`

### Python script debugging — check for hidden instances first
```
netstat -ano | grep :5000
tasklist | grep -i python
```
Kill all: `powershell -Command "Get-Process python,pythonw | Stop-Process -Force"`
Always run the script in a visible `cmd` window during debugging.

### Start NORA Player — full startup sequence
`C:\Users\Ohad\Desktop\Start NORA Player.bat`

1. **Kill Python → start `youtube_player.py`** — always a clean single instance.
2. **Verify Python** — HTTP GET to `localhost:5000/thumbnail`. 200 or 404 = alive.
3. **Kill IAR + JLink.exe + JLinkGDBServer + RTT Viewer** — J-Link allows only ONE client.
4. **`cspybat --download_only`** — flashes firmware with exact IAR project settings.
5. **Kill `JLink.exe` again after flash** — `cspybat` leaves probe held after exit.
   Without this second kill, RTT Viewer fails with "Cannot connect to probe".
6. **1-second pause** — OS time to release USB device handle.
7. **Open RTT Viewer** — RTT ring buffer holds all boot logs until drained (no logs lost).

**Log file:** `%TEMP%\nora_player.log`
