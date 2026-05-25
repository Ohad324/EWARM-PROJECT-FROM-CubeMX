# Voice-to-Music Pipeline

**Last updated:** 2026-05-23

End-to-end architecture for the "Hey Noa, play X" voice command flow — from wake-word detection on STM32 to YouTube playback on the PC, including thumbnail rendering on the LCD.

---

## Architecture overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│  STM32H747I-DISCO (CM7 @ 400 MHz)                                       │
│                                                                          │
│   Mic (MP34DT05-A)                                                       │
│      │ PDM                                                               │
│      ▼                                                                   │
│   DFSDM1 Filter0 (Sinc3, OSR=125) ──► 16 kHz PCM                         │
│      │                                                                   │
│      ▼                                                                   │
│   Edge Impulse "Hey Noa" classifier (rolling 1-sec window @ 2 Hz)        │
│      │                                                                   │
│      ▼ wake fires (pct >= 85)                                            │
│   Record 3 sec WAV → write to SD                                         │
│      │                                                                   │
│      ▼ UART8 @ 921600                                                    │
└──────┼──────────────────────────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────────────────┐
│  NORA (ESP32-S3 W106 module, BLE released, WiFi only)                   │
│                                                                          │
│   UART buffer (96 KB static BSS, NO malloc)                              │
│      │                                                                   │
│      ▼ HTTPS PUT                                                         │
│   Google Cloud Storage (gs://<bucket>/REC_NNNN.wav)                     │
│      │                                                                   │
│      ▼ HTTPS POST                                                        │
│   ┌───── Google Speech-to-Text ─────┐                                    │
│   │  Call #1: en-US primary         │                                    │
│   │  Probe — does transcript        │                                    │
│   │  start with "play "?            │                                    │
│   │     YES → Path A (done, 1 call) │                                    │
│   │     NO  → Path B (2nd call)     │                                    │
│   │  Call #2: iw-IL primary         │                                    │
│   └─────────────────────────────────┘                                    │
│      │                                                                   │
│      ▼ transcript                                                        │
│   cmd_router (Rule A1: "play X" → music_request("X"))                    │
│      │                                                                   │
│      ▼                                                                   │
│   music_task → ntfy_search()                                             │
│      │                                                                   │
│      ▼ HTTPS POST                                                        │
└──────┼──────────────────────────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────────────────┐
│  ntfy.sh public broker (bypasses iPhone hotspot client-isolation)        │
│                                                                          │
│   Topic: hey-noa-7f3a9b2c-req  (NORA publishes, bridge subscribes)       │
│   Topic: hey-noa-7f3a9b2c-res  (bridge publishes, NORA polls)            │
└──────┬──────────────────────────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────────────────┐
│  PC (Windows)                                                            │
│                                                                          │
│   bridge.py (subscribed via SSE to /req)                                 │
│      │ receives {"id":N,"query":"..."}                                   │
│      ▼ HTTP POST                                                         │
│   youtube_player.py at localhost:5000/play                               │
│      │ scrapes YouTube, opens Chrome with first result                   │
│      ▼ returns {"status":"ok","videoId":"..."}                           │
│   bridge.py POSTs result back to /res                                    │
└──────┬──────────────────────────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────────────────┐
│  NORA: ntfy_search() polls /res, picks up matching id                    │
│      │ builds thumbnail URL = img.youtube.com/vi/<videoId>/mqdefault.jpg │
│      ▼ UART8 → STM32                                                     │
│   "TRACK:<title>||<thumb_url>"                                           │
│   "THUMB: <jpeg bytes>"                                                  │
└──────┬──────────────────────────────────────────────────────────────────┘
       │
┌──────▼──────────────────────────────────────────────────────────────────┐
│  STM32: TouchGFX MusicScreen renders thumbnail via PFB strip pipeline    │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## STT bilingual routing — Path A vs Path B

The first STT call is a **language probe** — its only job is to decide which path to commit to. Neither language is the "primary" or "fallback"; both are first-class.

```
                ┌────────────────────────────────────────────┐
                │  STT Call #1                               │
                │    languageCode = "en-US"                  │
                │    alternativeLanguageCodes = ["iw-IL"]    │
                │  Duration: ~3 sec                          │
                └────────────────┬───────────────────────────┘
                                 │ transcript
                                 ▼
                       ┌─────────────────────┐
                       │ Starts with "play "?│
                       └──────┬───────┬──────┘
                              │ YES   │ NO
                              ▼       ▼
                    ┌─────────────┐   ┌────────────────────────────────────┐
                    │   Path A    │   │  STT Call #2                       │
                    │   ─────     │   │    languageCode = "iw-IL"          │
                    │  Use this   │   │    alternativeLanguageCodes =      │
                    │  transcript │   │      ["en-US"]                     │
                    │   DONE      │   │  Duration: ~3 sec                  │
                    │             │   │  (Discard Call #1 result)          │
                    │             │   └─────────────┬──────────────────────┘
                    │             │                 │ transcript
                    │             │                 ▼
                    │             │           ┌─────────────────────┐
                    │             │           │      Path B         │
                    │             │           │      ─────          │
                    │             │           │   Use this Hebrew   │
                    │             │           │   transcript        │
                    │             │           │      DONE           │
                    │             │           └─────────────────────┘
                    └─────────────┘
```

**Why two sequential calls instead of parallel?** Two concurrent TLS handshakes on ESP32-S3 exhaust the heap and trigger `MBEDTLS_ERR_SSL_ALLOC_FAILED` (see [BUGS_OPEN_2026-05-18_NORA_WAKE_WORD.md](BUGS_OPEN_2026-05-18_NORA_WAKE_WORD.md#bug-013--tls-heap-exhaustion-on-overlapping-voice-commands) BUG-013). Sequential keeps exactly one TLS session alive at any moment.

**Why English first?** The grammar `"play <X>"` is always English-prefixed in this project's command structure. English-primary STT recognises "play" reliably; Hebrew-primary STT often drops it as unrecognised Hebrew gibberish.

---

## End-to-end latency

| Stage | Path A (English command) | Path B (Hebrew / bare name) |
|---|---|---|
| Wake-word + 3-sec recording | 3.0 sec | 3.0 sec |
| UART stream STM32→NORA | 1.0 sec | 1.0 sec |
| GCS upload (TLS handshake + body) | 1.0 sec | 1.0 sec |
| **STT call #1 (en-US probe)** | **2.5 sec** | **2.5 sec** |
| STT call #2 (iw-IL) | — | 2.5 sec |
| ntfy publish + poll for videoId | 1.0 sec | 1.0 sec |
| Bridge → youtube_player → Chrome opens | 1.0 sec | 1.0 sec |
| **Total: "Hey Noa" → music starts** | **~7-8 sec** | **~10-11 sec** |

Path B's 3-sec penalty IS the cost of figuring out the language wasn't English.

---

## Cost per command (Google Cloud)

| Item | Cost | Notes |
|---|---|---|
| GCS upload + storage | < $0.0001 | 96 KB WAV, deleted after 1 day |
| STT call #1 | $0.006 | Standard rate for short utterances (<60 sec) |
| STT call #2 (Path B only) | $0.006 | Only when English probe didn't find "play " |
| **Path A total per command** | **~$0.006** | |
| **Path B total per command** | **~$0.012** | 2x because of the language fallback |

At 100 voice commands/day, that's ~$0.60 to $1.20 in GCP charges.

---

## Components & files

### STM32 firmware (CM7)
| Component | File | Role |
|---|---|---|
| Wake-word classifier | [CM7/Core/Src/wake_word_test.cpp](../CM7/Core/Src/wake_word_test.cpp) | Edge Impulse Hey Noa model, 2 Hz @ idle hook |
| Audio recording | [CM7/Core/Src/voice_recorder.c](../CM7/Core/Src/voice_recorder.c) | DFSDM capture + WAV save + UART stream |
| UART send | [CM7/Core/Src/uart_send_task.c](../CM7/Core/Src/uart_send_task.c) | UART8 stream to NORA |
| TouchGFX MusicScreen | [CM7/TouchGFX/gui/src/screen2_screen/Screen2View.cpp](../CM7/TouchGFX/gui/src/screen2_screen/Screen2View.cpp) | Renders track title + thumbnail |

### NORA firmware (ESP32-S3)
| Component | File | Role |
|---|---|---|
| UART RX | [NORA_BLE/.../nora_ble_bridge.c](../NORA_BLE/nora_wifi_ble_translate/main/nora_ble_bridge.c) | DMA RX from STM32, dispatches AUDIO:FILE etc. |
| WiFi multi-AP | [NORA_BLE/.../nora_ble_bridge.c:65-72](../NORA_BLE/nora_wifi_ble_translate/main/nora_ble_bridge.c#L65) | Scans + connects to known APs (`iPhone`, `Ohad2.4`, `Sightsys_SEC24`) |
| GCS upload + STT | [NORA_BLE/.../cloud_upload.c](../NORA_BLE/nora_wifi_ble_translate/main/cloud_upload.c) | Bilingual 2-call STT lives here |
| cmd_router | [NORA_BLE/.../command_router.c](../NORA_BLE/nora_wifi_ble_translate/main/command_router.c) | Rule A1 matches `"play <X>"` → music_request |
| music_task + ntfy search | [NORA_BLE/.../music_task.c](../NORA_BLE/nora_wifi_ble_translate/main/music_task.c) | Queue, dispatches search via `ntfy_search()` |
| ntfy.sh relay client | [NORA_BLE/.../ntfy_client.c](../NORA_BLE/nora_wifi_ble_translate/main/ntfy_client.c) | Publishes /req, polls /res |

### PC tools (outside git repo)
| Component | Path | Role |
|---|---|---|
| ntfy.sh bridge | `C:\Tools\ntfy-music-bridge\bridge.py` | SSE subscribes /req → forwards to localhost:5000/play → publishes /res |
| Task Scheduler install | `C:\Tools\ntfy-music-bridge\install_task.ps1` | Registers bridge as logon task |
| README | `C:\Tools\ntfy-music-bridge\README.md` | Setup + troubleshooting |
| YouTube player | [tools/youtube_player.py](../tools/youtube_player.py) | Scrapes YouTube, opens Chrome, serves /play /command /thumbnail |
| VS Code task | `.vscode/tasks.json` → "NORA Bridge (ntfy.sh relay…)" | One-click start with auto-kill of prior instances |

---

## Configuration & magic strings

| Item | Where | Value |
|---|---|---|
| ntfy.sh request topic | `ntfy_client.c` `NTFY_TOPIC_REQ` AND `bridge.py` `REQ_TOPIC` | `hey-noa-7f3a9b2c-req` |
| ntfy.sh response topic | `ntfy_client.c` `NTFY_TOPIC_RES` AND `bridge.py` `RES_TOPIC` | `hey-noa-7f3a9b2c-res` |
| ntfy.sh poll window | `ntfy_client.c` `NTFY_SINCE_WINDOW` | `"20s"` |
| ntfy.sh poll interval | `ntfy_client.c` `NTFY_POLL_INTERVAL_MS` | `250` |
| ntfy.sh overall timeout | `music_task.c` argument to `ntfy_search()` | `15000` ms |
| Wake-word threshold | `wake_word_test.cpp` `WAKEWORD_THRESHOLD_PCT` | `85` |
| Recording length | `voice_recorder.c` | 3 sec @ 16 kHz |
| PC search endpoint | `bridge.py` `PC_PLAY_URL` | `http://127.0.0.1:5000/play` |
| GCS bucket | NORA `CLOUD_GCS_BUCKET` | set via build config |
| STT primary lang | `cloud_upload.c` try_stt_call args | en-US then iw-IL |

**To rotate ntfy topics** (e.g. if you suspect they leaked): change both files at the marked constants and rebuild NORA. Topics are random hex so collisions on the public ntfy.sh service are virtually impossible.

---

## How the iPhone-hotspot bypass works

iPhone Personal Hotspot enforces **AP client isolation** — devices on the hotspot can reach the public internet but NOT each other. So `NORA → PC` direct LAN traffic is silently dropped (was BUG-004, blocking the previous architecture).

Both NORA and bridge.py make only **outgoing** HTTPS calls to `ntfy.sh`, which the iPhone permits. They never try to talk to each other directly. ntfy.sh acts as a public meeting point that both can reach.

```
                  iPhone blocks this:
NORA ────────────╳────────────► PC

                  Both options use this path instead:
NORA ──► public internet ──► ntfy.sh ──► public internet ──► PC      ✅
```

---

## Known issues & limitations

| ID | Issue | Status |
|---|---|---|
| [BUG-013](BUGS_OPEN_2026-05-18_NORA_WAKE_WORD.md#bug-013--tls-heap-exhaustion-on-overlapping-voice-commands) | TLS heap exhaustion if two voice commands within ~15 sec | Open — workaround: wait between commands |
| [BUG-010](BUGS_OPEN_2026-05-18_NORA_WAKE_WORD.md#bug-010--audio-quality-snr-0-db-dc-offset-924-speech-barely-above-noise) | Audio SNR ~0 dB, DC offset 924 | Open — affects STT accuracy on quiet speech |
| [BUG-006](BUGS_OPEN_2026-05-18_NORA_WAKE_WORD.md#bug-006--speakermic-acoustic-bleed-corrupts-wake-word-and-commands) | Speaker → mic acoustic bleed | Open — proposed: audio ducking via ntfy on wake |
| [BUG-005](BUGS_OPEN_2026-05-18_NORA_WAKE_WORD.md#bug-005--wake-word-false-triggers-on-musicambient-noise) | False wake-word triggers on noise | Open — threshold 85 mitigates partially |
| Hebrew commands without "play"/"נגן" | cmd_router only matches `"play X"` — bare names rejected as unknown | Open — proposed "Option A" rule (bare-name = song search) |
| LCD font lacks Hebrew glyphs | TouchGFX font may show `?` for Hebrew transcripts | Open — needs Hebrew-supporting font added to TouchGFX |

---

## Test recipes

### Path A test (English command)
1. Start bridge: VS Code → `Tasks: Run Task` → `NORA Bridge (ntfy.sh relay…)`
2. Wait for `[bridge] connected to hey-noa-7f3a9b2c-req`
3. Say: `"Hey Noa, play Beatles"`
4. Expected NORA log:
   ```
   Probe[en-US]: "play Beatles"
   ntfy: publish OK id=XXX query="Beatles"
   ntfy: got response id=XXX videoId=YYY
   ```
5. Expected bridge log:
   ```
   [bridge] req id=XXX query='Beatles'
   [bridge] res id=XXX videoId='YYY'
   ```
6. Expected on PC: Chrome opens to the Beatles YouTube page
7. Expected on STM32: thumbnail renders on LCD

### Path B test (Hebrew / bare name)
1. Same setup
2. Say: `"Hey Noa, Zohar Argov"`
3. Expected NORA log:
   ```
   Probe[en-US]: "Zohar Argov"          ← or something without "play"
   No "play " detected — routing to Hebrew STT (Path B)
   Transcript[iw-IL]: "Zohar Argov"     ← or Hebrew chars
   ntfy: publish OK ...
   ```
4. Rest of pipeline identical to Path A

### BUG-013 reproduction (do NOT do this in normal use)
1. Say two commands within 10 seconds of each other
2. Watch NORA log for `mbedtls_ssl_setup returned -0x7F00` cascade after the second wake
3. Second command will fail; system recovers on third (after first poll times out)

---

## Glossary

- **Path A** — STT call #1 returns "play X". One STT call used. Fast (~7-8s).
- **Path B** — STT call #1 doesn't return "play X". Discard, run call #2 in Hebrew. Two calls used (~10-11s).
- **Probe** — STT call #1's role. Even though it transcribes, its primary purpose is language detection. Its transcript is **discarded** if Path B is chosen.
- **BUG-013** — ESP32-S3 mbedTLS heap exhaustion when two TLS sessions live concurrently.
- **AP client isolation** — iPhone hotspot security feature that drops device-to-device traffic. Why ntfy.sh relay exists.
