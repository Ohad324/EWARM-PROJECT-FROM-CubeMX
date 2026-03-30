# NORA Bug List

## Open Bugs

### BUG-001 — YouTube scraper fails due to Unicode arrow in print statement
**File:** `tools/youtube_player.py` — `_youtube_search_first_id()`
**Severity:** High — blocks correct thumbnail and song from showing on STM32 LCD
**Symptom:** Every search falls back to Rick Astley (hardcoded videoId `dQw4w9WgXcQ`).
**Root cause:** The print statement uses `→` (`\u2192`) which Windows cp1252 codec
cannot encode when writing to the log file. The exception is caught by the surrounding
`except` block, causing the function to return `None` even though the videoId was
successfully found.
**Fix:** Replaced `→` with `->` in the print statement (1-character change).
**Status:** Fixed

---

### BUG-006 — Rick Astley opens on PC and LCD when search fails
**File:** `tools/youtube_player.py` — `do_POST /play handler`
**Severity:** High — wrong song plays and wrong thumbnail shows on LCD
**Symptom:** Any search that fails the YouTube scrape opens Rick Astley
(`dQw4w9WgXcQ`) on Chrome and sends his thumbnail to the STM32 LCD.
**Root cause:** A hardcoded fallback videoId was added as temporary scaffolding
to test the STM32 thumbnail pipeline. It was never meant to stay in production.
**Fix:** Remove the hardcoded fallback. If scraping fails, return an error response
(no videoId) so NORA knows not to send a THUMB. The real fix is BUG-001 (Unicode)
which will make the scraper work correctly and eliminate the need for any fallback.
**Status:** Fixed — fallback removed, BUG-001 fix makes scraper work correctly

---

### BUG-004 — Old YouTube window keeps playing when new song is requested
**File:** `tools/youtube_player.py` — `open_youtube()`
**Severity:** High — old song keeps playing, new song opens silently
**Symptom:** Playing "Beethoven" while "Mozart" is playing — Mozart continues,
Beethoven opens in a new window but is silent.
**Root cause:** Not yet diagnosed. Candidates:
  1. `_chrome_proc` PID tracking failing — old window not being terminated
  2. Chrome with real profile (`--new-window`) opens a new tab in existing instance
     instead of a new trackable process — `terminate()` kills the launcher, not the window
  3. The new window opens without focus → Chrome mutes it
**Status:** Open — needs diagnosis

---

### BUG-007 — Thumbnail JPEG opens in PC image viewer on every PLAY
**File:** `tools/youtube_player.py` — `_fetch_and_open_thumbnail()`
**Severity:** Low — annoying, unnecessary
**Symptom:** Every time a song is played, the thumbnail JPEG pops up in the PC's
default image viewer (Windows Photos / Paint etc.).
**Root cause:** `_open_file(THUMB_PATH)` was added for early debugging to visually
confirm the thumbnail was downloaded. Not needed in production.
**Fix:** Remove the `_open_file(THUMB_PATH)` call. Thumbnail is still saved to disk
and served via `/thumbnail` endpoint to NORA — nothing else changes.
**Status:** Fixed

---

### BUG-005 — New PLAY opens a new Chrome window instead of a new tab
**File:** `tools/youtube_player.py` — `open_youtube()`
**Severity:** Medium — opens in wrong Chrome profile (not premium), clutters the screen
**Symptom:** Each PLAY opens a separate Chrome window in the NORA profile instead of
a new tab in the user's existing Chrome session (which has YouTube Premium).
**Desired behavior:** Open a new tab in the existing Chrome window using the user's
real profile — same as typing a YouTube URL into the address bar.
**Fix:** Remove `--new-window` and `--user-data-dir` flags. Without these, Chrome
opens a new tab in the existing window using the real profile.
Trade-off: lose ability to auto-close previous tab (tabs can't be tracked by PID).
**Status:** Open

---

### BUG-002 — Chrome audio silent on first PLAY (new NORA profile)
**File:** `tools/youtube_player.py` — `open_youtube()`
**Severity:** Medium — YouTube opens but plays silently
**Symptom:** Chrome opens YouTube with no audio until user clicks the video.
**Root cause:** NORA uses a separate Chrome profile (`nora_chrome_profile`) with
Media Engagement Index (MEI) = 0 for YouTube. Chrome mutes autoplay audio for sites
with low MEI score.
**Fix:** `_ensure_youtube_audio_allowed()` writes a Chrome Preferences file granting
YouTube sound permission before each launch. May need verification across Chrome versions.
**Status:** Under test

---

### BUG-003 — YouTube API key blocked (HTTP 400)
**File:** `NORA_BLE/nora_wifi_ble_translate/main/music_task.c` — `youtube_search()`
**Severity:** Medium — workaround in place via `search_via_pc()`
**Symptom:** Every YouTube API call returns HTTP 400 "API key not valid".
**Root cause:** Current API key is permanently blocked.
**Fix:** Replace with a valid YouTube Data API v3 key in `music_task.c`.
**Status:** Open — workaround active (`search_via_pc` fallback)

---

## Closed Bugs

### CLOSED-001 — STM32 LCD black on boot (SD card crash)
**Fixed in:** `CM7/Core/Src/audio_sd.c`
`AudioSD_Init()` called `HAL_SD_Init()` before checking card presence → `Error_Handler()` → system freeze.
Fix: check SD_DETECT pin first, make HAL_SD_Init failure non-fatal.

### CLOSED-002 — UART DMA BUSY on boot (ORE overrun)
**Fixed in:** `CM7/Core/Src/ble_uart.c`
NORA sends data before STM32 arms DMA → ORE flag set → DMA handle not READY → HAL_BUSY.
Fix: clear ORE + abort DMA handle before arming.

### CLOSED-003 — NORA waits for PC acknowledgment before streaming to STM32
**Fixed in:** `NORA_BLE/nora_wifi_ble_translate/main/music_task.c`
`post_to_pc_player()` was called before UART mutex → STM32 waited for PC HTTP round-trip.
Fix: moved `post_to_pc_player()` to after `xSemaphoreGive(s_uart_mutex)`.
