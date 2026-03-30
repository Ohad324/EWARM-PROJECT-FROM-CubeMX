# CLAUDE.md — MyApplication Project Instructions

## Project Overview
STM32H747I-DISCO music display system:
- **iPhone** → BLE "PLAY:song" → **NORA (ESP32)** → YouTube API → UART8 → **STM32H747** → LCD thumbnail + title
- **NORA** → HTTP POST → **youtube_player.py** (PC) → Chrome opens YouTube

## IAR Project
- Project file: `EWARM/STM32H747I-DISCO.ewp`
- Workspace: `EWARM/STM32H7- 7.20 - Disco.eww`
- Active core: **CM7** (`STM32H747I-DISCO_CM7` build target)
- Board: STM32H747I-DISCO (dual-core H747, CM7 + CM4)

## General Rules

### Think before you act — examine the problem first
Before writing any code or making any change, pause and do the following:
1. **Understand the root cause** — read the relevant code, check the logs, reproduce the problem in your head
2. **List all options** — consider every possible solution, not just the first one that comes to mind
3. **Present options to the user** — explain the trade-offs of each before picking one
4. **Get confirmation** — agree on the approach with the user before implementing

Do NOT jump to writing code the moment a problem is described.
If the first solution fails, do NOT immediately try another — go back to step 1 and re-examine.
A wrong solution applied quickly is worse than a correct solution applied after thinking.

### Work log
Document every step taken in `doc/worklog.md` — what was changed, why, and what's pending.
Read it at the start of each session to resume where we left off.
Update `doc/worklog.md` every 10 minutes with a timestamp and what was done/changed.

### Commit every 10 minutes
During any working session, commit all changes every 10 minutes automatically.
Use a short descriptive commit message summarizing what changed.

## Debugging Rules

### Always use HARDWARE breakpoints — never software
In all C-SPY `.mac` files, always use `__setHWCodeBreak()` instead of `__setCodeBreak()`.
Software breakpoints patch flash with BKPT instructions — they corrupt the image and can
cause false halts or miss hits in ISR/HAL code. The Cortex-M7 FPB unit has 8 hardware
BP registers, which is enough for all debug sessions in this project.

## Critical Rules

### NORA must NOT wait for PC acknowledgment before streaming to STM32
The PC player (youtube_player.py / Chrome) is for playback only — it must never block
the STM32 pipeline. The mandatory execution order in music_task.c is:
1. `youtube_search()` OR `search_via_pc()` — resolve videoId + thumbnail URL
2. Take UART mutex → send TRACK to STM32 → stream THUMB to STM32 → release mutex
3. `post_to_pc_player()` — notify PC AFTER STM32 is done (fire-and-forget)

When `search_via_pc()` is used (API broken), the PC wait is for getting the videoId
(data we need), NOT for PC acknowledgment. Chrome opens on the PC as a side-effect of
that call — no separate post_to_pc_player() call is needed in that path.
When a working YouTube API key is used, youtube_search() provides the videoId with zero
PC dependency and post_to_pc_player() is purely fire-and-forget after STM32 is done.

### Why the SD card init was crashing the system
`AudioSD_Init()` calls `SDMMC1_GPIO_Init()` then immediately `SDMMC1_Peripheral_Init()`,
which calls `HAL_SD_Init()`. If no SD card is inserted, `HAL_SD_Init()` returns `HAL_ERROR`
and the code called `Error_Handler()` — which disables all interrupts and loops forever.
This happened BEFORE the card-detect check (PI8), so the system crashed on every boot
without an SD card, making the LCD completely dark.

**Fix applied (audio_sd.c):**
1. `SDMMC1_GPIO_Init()` first (so SD_DETECT pin PI8 is readable)
2. Check SD_DETECT — if card absent, return false immediately
3. Only then call `SDMMC1_Peripheral_Init()`
4. Made `HAL_SD_Init()` failure non-fatal (logs `SD:INIT_FAIL`, no crash)

**Rule:** Never call `Error_Handler()` inside peripheral init functions that are
called unconditionally from `main()`. Always check hardware presence first.

### Why BLE_UART_StartDMA was crashing (ORE / DMA BUSY)

**Symptom:** System boots, UART task starts, hits `Error_Handler` immediately.
Call stack: `Error_Handler ← BLE_UART_StartDMA ← UARTReceiveTask`.
IAR Watch shows `huart8.ErrorCode = 8 (ORE)`, `gState = READY`, `RxState = READY`.

**Root cause:** NORA (ESP32) sends UART data continuously during STM32 boot.
Before `BLE_UART_StartDMA()` arms the DMA, bytes arrive on UART8.
The UART peripheral has no DMA running yet → bytes overflow the FIFO →
ORE (Overrun Error, code 0x08) is set in hardware AND in `huart8.ErrorCode`.
The ORE leaves the internal DMA handle (`s_hdma_uart8_rx`) in a non-READY state.
When `HAL_UARTEx_ReceiveToIdle_DMA` calls `HAL_DMA_Start_IT`, it finds the
DMA handle not READY → returns HAL_BUSY → outer function returns HAL_ERROR.

**Fix applied (ble_uart.c — BLE_UART_StartDMA):**
```c
__HAL_UART_CLEAR_FLAG(&huart8, UART_CLEAR_OREF | UART_CLEAR_FEF | ...);
huart8.ErrorCode = HAL_UART_ERROR_NONE;
huart8.RxState   = HAL_UART_STATE_READY;
HAL_DMA_Abort(huart8.hdmarx);   /* resets DMA handle State to READY */
```
Then call `HAL_UARTEx_ReceiveToIdle_DMA` — no `Error_Handler()` on failure.

**Rule:** Never call `Error_Handler()` from `BLE_UART_StartDMA`. Always clear
ORE + abort DMA handle before arming. NORA will always send data before STM32
is ready — this is expected and must be handled gracefully.

### AudioRec is DISABLED — do not re-enable
`AudioRec_TaskEntry` must remain commented out in `main.c`.
DFSDM/GPIO conflict causes a crash before the thumbnail pipeline can display anything.
Only re-enable after the thumbnail pipeline is confirmed stable.

### Python script — always restart after changes
After every change to `tools/youtube_player.py`, restart it:
```
powershell -Command "Get-Process python,pythonw -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1; Start-Process python -ArgumentList 'C:\TouchGFXProjects\MyApplication\tools\youtube_player.py' -WindowStyle Normal"
```
Then verify it's running: `netstat -ano | grep :5000`

### Python script debugging — check for hidden instances first
Before changing any code, always check for hidden processes stealing port 5000:
```
netstat -ano | grep :5000
tasklist | grep -i python
```
Kill all: `powershell -Command "Get-Process python,pythonw | Stop-Process -Force"`
Always run the script in a visible `cmd` window during debugging.

### Start NORA Player — desktop bat file
`C:\Users\Ohad\Desktop\Start NORA Player.bat`

One double-click runs the full test-session startup sequence.
Double-click before every test — it handles everything so you can go straight to sending PLAY.

**Sequence and why each step is in this order:**

1. **Kill Python → start `youtube_player.py`** — always a clean single instance.
   stdout/stderr saved to `%TEMP%\nora_player.log` so crashes are visible later.

2. **Verify Python** — real HTTP GET to `localhost:5000/thumbnail`.
   A `200` or `404` both mean the server is alive. Aborts here if server failed to start.

3. **Kill IAR + JLink.exe + JLinkGDBServer + RTT Viewer** — J-Link allows only ONE
   client at a time. Any of these holding the probe will block the flash step.
   All killed here for a guaranteed clean slate before touching the hardware.

4. **`cspybat --download_only`** — flashes firmware using the exact IAR project
   settings (device, SWD, reset strategy). We keep `cspybat` (not JLinkExe) to
   stay consistent with the IAR toolchain — same tool IAR uses internally.

5. **Kill `JLink.exe` again after flash** — `cspybat --download_only` leaves the
   J-Link probe held even after it exits: the board hits `main()` and the probe
   stays connected. Without this second kill, RTT Viewer fails with
   "Cannot connect to probe". This is the key step that makes the sequence reliable.

6. **1-second pause** — gives the OS time to fully release the USB device handle
   before RTT Viewer claims it.

7. **Open RTT Viewer** — connects to the now-free J-Link. The STM32 RTT ring buffer
   holds all log entries from boot until RTT Viewer drains them — no logs are lost
   even though RTT Viewer connects a few seconds after reset.

After the bat finishes: Python on port 5000, RTT Viewer capturing logs, STM32
running latest firmware. Send PLAY from iPhone.

**Log file:** `%TEMP%\nora_player.log` — check if Chrome stops opening or NORA
reports `ESP_ERR_HTTP_CONNECT`.

**Tool paths:**
- cspybat:    `C:\iar\ewarm-9.70.2\common\bin\cspybat`
- RTT Viewer: `C:\Program Files\SEGGER\JLink_V930a\JLinkRTTViewer.exe`

## Key Files
| File | Purpose |
|------|---------|
| `CM7/Core/Src/main.c` | UARTReceiveTask, THUMB state machine, task creation |
| `CM7/Core/Src/music_display_task.c` | JpegDisplayTask, JPEG decode, xMusicQueue |
| `CM7/Core/Src/ble_uart.c` | DMA+IDLE receive, 128-byte bursts → xRawBleQueue |
| `CM7/Core/Src/audio_sd.c` | SD card WAV recording (new) |
| `CM7/Core/Src/command_handler.c` | CMD: message routing (new) |
| `tools/youtube_player.py` | PC HTTP server — receives NORA POSTs, opens Chrome |
| `NORA_BLE/nora_wifi_ble_translate/main/nora_ble_bridge.c` | WiFi credentials at lines 57–58 |
| `NORA_BLE/nora_wifi_ble_translate/main/music_task.c` | YouTube search, UART send to STM32 |

## UART Protocol (NORA → STM32 over UART8, 921600 baud)
```
TRACK:<title>|<artist>|<videoId>\n
THUMB:<size>\n + binary JPEG bytes
ERROR:<reason>\n
RESULT:<word>\n   — Hebrew translation (existing pipeline)
CTRL:<action>\n   — Phase 2: play/pause/next/prev (stubbed)
```

## Build & Flash
1. **ESP32**: `idf.py build flash monitor` from `NORA_BLE/nora_wifi_ble_translate/`
2. **STM32**: IAR → Build → Download and Debug (CM7 target)
3. **PC**: `python tools/youtube_player.py` (must be running before playing)

## Before Building STM32
Check `EWARM/STM32H747I-DISCO.ewp` — `thumb_pipeline.c` and `thumb_ref_jpeg.c` were deleted from disk but may still be listed. Remove them if present or IAR will error.
