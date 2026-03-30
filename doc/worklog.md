# Work Log

## 2026-03-30

### Session — afternoon

#### youtube_player.py fixes
- Added comments throughout `tools/youtube_player.py` to clarify both entry points (`/play` for NORA, `/command` for voice)
- Created desktop shortcut at `C:\Users\Ohad\OneDrive - sightsys\Desktop\Phyton script\YouTube Player.lnk`
- **Bug fix:** Multiple hidden `pythonw.exe` instances were stealing HTTP requests on port 5000 — killed all, restarted clean
- **Bug fix:** Chrome window replacement — switched from `--new-window` (untrackable, opens extra windows) to `--user-data-dir=nora_chrome_profile` (independent process, trackable and terminatable)
- **Bug fix:** YouTube opening silent — root cause: NORA in "BLE search mode" sends query (no videoId), script was opening YouTube search results page (no video playing). Fix: added `_youtube_search_first_id()` which scrapes YouTube search results and extracts the first videoId, then opens that video directly with `&autoplay=1`
- Added `--autoplay-policy=no-user-gesture-required` Chrome flag to allow autoplay with sound on fresh profile

#### CLAUDE.md created
- Created `CLAUDE.md` at project root with permanent instructions:
  - Commit every 10 minutes
  - AudioRec must stay disabled
  - Always restart python script after changes
  - Always check port 5000 for hidden instances before debugging
  - Key files reference, UART protocol, build order

#### worklog.md created (this file)
- Added per-session documentation so work can be resumed after session restart

#### STM32 thumbnail regression — identified, fix pending
- **Symptom:** LCD shows nothing when a track is played
- **Root cause:** `AudioRec_TaskEntry` was re-enabled in `main.c` as part of new SD card work. DFSDM/GPIO conflict in AudioRec crashes the pipeline before thumbnail can display.
- **Fix applied:** Commented out `xTaskCreate(AudioRec_TaskEntry, ...)` in `main.c`
- **Status:** Fix written, STM32 not yet rebuilt/flashed — waiting for user

#### What needs to happen next
1. Rebuild STM32 CM7 in IAR and flash → verify thumbnail shows on LCD
2. Once thumbnail confirmed working, continue with SD card voice recording feature

---

## 2026-03-30 Evening Session (19:00–20:00)

### Plan
| Time | Goal |
|------|------|
| 19:00–19:20 | Fix LCD thumbnail — rebuild + flash STM32 with AudioRec disabled |
| 19:20–19:50 | Speech-to-text pipeline — fix AudioRec alongside thumbnail, wire button→record→NORA→CMD |
| 19:50–20:00 | End-to-end test |

### Speech-to-text architecture
1. STM32 mic (DFSDM) → records WAV → saves to SD card (`audio_sd.c`)
2. STM32 sends WAV over UART8 to NORA (`AUDIO:FILE:REC_001.wav:<size>\n` + binary)
3. NORA does speech-to-text → sends `CMD:<text>\n` back to STM32
4. `CommandHandler` on STM32 dispatches the command to the screen

### 19:30 — Second crash found: AudioRec_Init() still active
- We had commented out `xTaskCreate(AudioRec_TaskEntry)` but NOT `AudioRec_Init()`
- `AudioRec_Init()` initializes DFSDM + GPIO + DMA and was calling `Error_Handler()`
- This caused `BLE_UART_StartDMA` to fail (UART DMA corrupted by DFSDM init)
- **Fix:** commented out `AudioRec_Init()` on line 274 of main.c
- DFSDM uses DMA1_Stream1, UART8 uses DMA1_Stream0 — no DMA conflict, but GPIO/DFSDM peripheral conflict still exists
- Waiting for rebuild + flash

### 19:10 — Root cause found and fixed: audio_sd.c crash
- `SDMMC1_Peripheral_Init()` was called before SD card detect check
- `HAL_SD_Init()` fails with no SD card → `Error_Handler()` → MCU locked, LCD dark
- **Fix:** moved SD_DETECT check before `SDMMC1_Peripheral_Init()` in `AudioSD_Init()`
- **Fix:** made `HAL_SD_Init()` failure non-fatal (logs `SD:INIT_FAIL` instead of crashing)
- Waiting for user to rebuild + flash

### 19:00 — Session started
- Read current state: `audio_sd.h`, `command_handler.h` already implemented
- AudioRec commented out in `main.c` (fix for LCD regression)
- Waiting for user to rebuild + flash STM32 in IAR

---

## 2026-03-30 Late Evening Session (~19:50+)

### Status at session resume
- STM32 boots cleanly (heartbeat idle, ISR=1)
- TRACK received from NORA ("metalica"), MusicScreen activated, title set
- LCD physically shows black — system is alive but display pipeline is not rendering
- AudioRec_Init() and AudioRec_TaskEntry both DISABLED (see CLAUDE.md)

### 19:50 — LCD debug session prepared
- Created `EWARM/lcd_debug.mac` — 5 C-SPY breakpoints covering the full display pipeline:
  - BP1: `TouchGFXHAL::taskEntry` — is TouchGFX task alive?
  - BP2: `OSWrappers::waitForVSync` — is it waiting for vsync?
  - BP3: `TouchGFXHAL::flushFrameBuffer` — is frame being pushed to DSI?
  - BP4: `HAL_DSI_EndOfRefreshCallback` — is DSI completing refresh?
  - BP5: `MusicScreenView::setupScreen` — is MusicScreen being set up?
- User instructed: load macro via IAR Project → Options → Debugger → Setup → Use macro file
- When each BP is hit: check FreeRTOS OS Awareness for task states
- **Waiting for user to run debug session and report which BP is hit**

---

---

> **⚠ SIDE NOTE — UNRESOLVED: C-SPY macro BPs not visible in IAR IDE**
>
> **Problem:** `__setHWCodeBreak()` calls inside `.mac` files set breakpoints that the MCU
> hits, but they do NOT appear in the IAR editor gutter or Breakpoints window.
> Only manually-placed BPs (F9 / right-click in editor) show up visually.
> The macro-placed BPs also seem unreliable — user reported only 3 of 5 hitting.
>
> **What was tried:**
> - Full path format `{C:\\TouchGFXProjects\\...\\ble_uart.c}272` — didn't resolve
> - Short filename format `{ble_uart.c}272` — IAR resolves the file, but BP still
>   invisible in the editor gutter
> - Software BPs (`__setCodeBreak`) are explicitly banned (CLAUDE.md rule) —
>   they patch flash with BKPT and corrupt ISR/HAL code
>
> **Root cause (suspected):** `__setHWCodeBreak()` registers BPs in the FPB hardware
> unit but bypasses IAR's internal BP list that drives the editor gutter display.
> IAR only shows BPs it "owns" (set via UI or `__setCodeBreak`). Hardware BPs set
> via macro are invisible to the IDE but still fire in hardware.
>
> **Possible solutions to investigate:**
> 1. **C-SPY `__setCodeBreak` with HW flag** — check if IAR 9.x `__setCodeBreak`
>    has an overload that marks the BP as hardware AND registers it in the IDE list
> 2. **execUserPreload() instead of execUserSetup()** — BPs set earlier in the
>    session lifecycle may register in the IDE list
> 3. **IAR Project → Breakpoints (saved BPs in .ewp/.ewd)** — BPs saved in the
>    project file via IDE persist across sessions and ARE shown in the gutter;
>    investigate if `.mac` can write to this list
> 4. **Manual BPs + `.mac` for conditional logic only** — user sets BP locations
>    manually (F9) for gutter visibility; `.mac` adds conditions/counts/actions
>
> **Current workaround:** User sets BPs manually in editor (F9). RTT logging used
> for pipeline tracing instead of BPs in ISR/DMA paths.
>
> **Status:** OPEN — needs investigation in the next debug session.

> **⚠ SIDE NOTE — USE J-LINK RTT VIEWER, NOT IAR TERMINAL I/O**
>
> **Finding:** IAR Terminal I/O was tried during this session and was NOT useful.
> It is too slow, drops output from fast DMA/ISR tasks, and is unreliable when
> FreeRTOS is running with high-priority tasks (UARTReceiveTask at AboveNormal).
>
> **J-Link RTT Viewer is the correct tool** — it uses a dedicated RAM ring buffer
> that the CPU writes to without blocking, and the J-Link probe drains it over SWD
> in the background. It never drops messages even from ISR context.
>
> **STM32H7xx_TRACE.dmac — REMOVED (2026-03-30)**
> The `--device_macro=STM32H7xx_TRACE.dmac` line was removed from all three
> `.xcl` files (CM7, CM4, cspy_cm7). The macro configures ETM/SWO trace pins
> (GPIOE/GPIOB) which are not wired on this board's J-Link — it failed every
> session with `Operation error` at line 359.
> Backup copy saved to: `EWARM/settings/disabled_macros/STM32H7xx_TRACE.dmac`
> Revert instructions in: `EWARM/settings/disabled_macros/README.txt`
> The other 4 device macros (H7xx, H7x5_M7, H7x5_DBG, H7xx_OB) are kept.
>
> **For every future debug session — START HERE:**
> 1. Open **J-Link RTT Viewer** (Start → SEGGER → J-Link RTT Viewer)
> 2. Device: `STM32H747XI` | Interface: `SWD` | Speed: `4000 kHz` | RTT Control Block: `Auto Detection`
> 3. Click OK — connects to live running board, no reset needed
> 4. All `LOG()` and `TLOG()` output appears here in real time

---

## 2026-03-30 Night Session (context resumed)

### Status
- BLE_UART_StartDMA ORE fix confirmed working — all tasks Blocked/healthy
- DSI callbacks confirmed firing (HAL_DSI_TearingEffectCallback + EndOfRefreshCallback)
- User confirmed "THUMB WAS SENT TO STM32" (from a run where youtube_search() succeeded)
- LCD still black despite THUMB arriving
- SD card was removed — SD:MOUNT_FAIL:3 log was from earlier SD-in test, not current issue

### Root cause analysis (code review completed)
Full pipeline traced and code is correct:
1. `UARTReceiveTask`: THUMB bytes → `s_jpegInBuf[]` → `SCB_CleanDCache` → `MSG_THUMB` → `xMusicQueue`
2. `JpegDisplayTask`: HAL_JPEG_Decode → YCbCr→RGB888 → scale 800x480 → `SCB_CleanDCache` → `MUSIC_DONE_THUMB` → `xMusicDoneQueue`
3. `Model::tick()`: if MusicScreen active → `onMusicThumbnail()` direct; else → cache + `onMusicPending()` → screen switch
4. `MusicScreenPresenter::activate()`: delivers cached TRACK + THUMB
5. `MusicScreenView::setThumbnail()`: `PixelDataWidget::setPixelData(rgb888)`, `setAlpha(255)`, `invalidate()`

### Next step: RTT log to find exact break point
RTT has full coverage — every step logs:
- `4 THUMB_accum X%` (every 10%)
- `[3] Thumbnail JPEG received`
- `5 MSG_THUMB_sent`
- `6 JPEG_Decode_start`
- `[JPEG] Stage1..6` + `[MUSIC] Pixel[0] #RRGGBB`
- `7 JPEG_Decode_done WxH st=0`
- `[Model] MUSIC_DONE_THUMB`
- `[Presenter] onMusicThumbnail`
- `[MusicView] setThumbnail`

**Action:** User opens J-Link RTT Viewer (SEGGER), plays BLE command, shares full log.
If `Pixel[0] = #000000` → JPEG corrupted. If step missing → break point found.
