# Voice Command Pipeline — Full Implementation Prompt

_Saved 2026-03-30_

---

## Context

Working STM32H7 audio recording system (`audio_rec.c`) that captures voice from a MEMS microphone
via DFSDM, converts PDM to PCM at 31,250 Hz 16-bit mono, and streams frames over UART8 to an
ESP32 (NORA).

---

## Feature Request

Implement a complete voice command pipeline across four stages:

### Stage 1 — Record to SD card (STM32 side)

- When the button is pressed to start recording, open a new `.wav` file on the SD card with a
  timestamped filename (e.g. `REC_001.wav`)
- Write a placeholder 44-byte WAV header at the start
- As each PCM frame arrives from the DMA (512 × int16 = 1024 bytes), append it to the file using FatFS
- When the button is pressed again to stop, seek back to byte 0 and overwrite the WAV header with
  the correct file size and data length values
- Close the file cleanly, then send `AUDIO:FILE:REC_001.wav\n` over UART8 to NORA
- The SD card interface pins are defined in the board schematics — check them and configure SDIO or
  SPI accordingly
- Use FatFS. If not yet initialised, add the initialisation

### Stage 2 — Upload to Google Cloud Storage (NORA/ESP32 side)

- On receiving `AUDIO:FILE:filename.wav\n`, upload the file to a Google Cloud Storage bucket via HTTPS
- Store bucket name and credentials as `#define` constants at the top of the file
- On success or failure send `UPLOAD:OK:filename.wav\n` or `UPLOAD:FAIL:filename.wav\n` back to
  STM32 over UART

### Stage 3 — Speech-to-text transcription (Google Cloud side)

- Immediately after upload, call the Google Cloud Speech-to-Text API on the uploaded file
- Audio format: WAV, 31,250 Hz, 16-bit, mono, LINEAR16 — pass these explicitly in the API request
- Store the Google API key as `#define SPEECH_API_KEY ""`
- The API will return a plain text transcript such as `"play Beatles"` or `"turn off the lights"`

### Stage 4 — Command routing (NORA/ESP32 side)

NORA receives the transcript string and parses it as a voice command, exactly the same way it
currently handles incoming NRF text commands. Implement a command router with the following rules:

**Rule A — YouTube / media commands** (e.g. `"play Beatles"`, `"stop music"`, `"next song"`):
- Detected by keywords: `play`, `stop`, `pause`, `next`, `previous`, `volume`
- NORA sends an HTTP POST request to a small server running on the PC at `http://PC_IP:PORT/command`
- Request body: `{"command": "play Beatles"}` as JSON
- The PC server (also to be written) listens on that port, parses the command, and controls YouTube
  or a media player accordingly
- Store `PC_IP` and `PORT` as `#define` constants

**Rule B — Screen / display commands** (e.g. `"show temperature"`, `"clear screen"`, `"update display"`):
- Detected by keywords: `show`, `display`, `screen`, `clear`, `update`
- NORA sends the command back to STM32 over UART as `CMD:show temperature\n`
- STM32 receives this in a new `CommandHandler` task, parses it, and updates the screen accordingly

**Rule C — Unknown commands:**
- NORA sends `CMD:UNKNOWN\n` to STM32 and logs the unrecognised transcript over debug serial

### PC-side server (Python)

Write a minimal Python HTTP server (using `http.server` or Flask) that:
- Listens for POST requests on `/command`
- Parses the `command` field from the JSON body
- For `play` commands: opens or controls YouTube in the browser using `webbrowser.open()` or sends
  keystrokes via `pyautogui`
- Prints all received commands to the terminal for debugging

---

## Constraints

- New SD card code goes in `audio_sd.c` / `audio_sd.h`
- New cloud + command routing logic on ESP32 goes in `cloud_upload.c` / `cloud_upload.h` and
  `command_router.c` / `command_router.h`
- New STM32 command receiver goes in `command_handler.c` / `command_handler.h`
- Do not break existing UART streaming, DMA double-buffer, or button debounce logic
- Handle errors at every stage: SD failure, WiFi drop, upload failure, API timeout, unknown command
  — log each over debug UART
- Comment every new function in the same style as the existing code
