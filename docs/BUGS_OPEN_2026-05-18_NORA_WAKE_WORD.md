# Open Bugs — NORA + STM32H7 wake-word pipeline

**Filed:** 2026-05-18
**Branch:** `test/inject-flasher-thumbnail-no-wifi`
**Context:** Issues identified during continuous wake-word testing sessions on 2026-05-17/18.
**Pipeline:** STM32H747 (wake-word + audio capture) → NORA-W106 (UART → Wi-Fi) → Google STT → cmd_router → PC search → STM32 LCD.

Each bug below is self-contained. Severity reflects user-impact on the voice-command UX.

---

## BUG-004 — iPhone hotspot blocks NORA→PC LAN HTTP (15 s timeouts)

**Severity:** **HIGH** — blocks every "play" command from completing successfully
**Status:** Open
**Workaround:** Use a real Wi-Fi router instead of iPhone Personal Hotspot

### Symptom

After STT returns a valid transcript, `search_via_pc` HTTP call to the local PC (`172.20.10.3:5000`) times out after 15 s. TRACK is sent to STM32 with empty videoId, thumbnail never loads, UI shows a broken track.

```
I (732195) cmd_router: Rule A1 → music_request: Led Zeppelin
I (732195) MUSIC: PLAY command queued: 'Led Zeppelin'
...
E (747215) esp-tls: [sock=54] select() timeout
E (747215) MUSIC: search_via_pc: open failed: ESP_ERR_HTTP_CONNECT
I (747225) MUSIC: TRACK sent: TRACK:Led Zeppelin||
```

15 000 ms = ESP-IDF default `esp_http_client` connect timeout.

### Root cause

**iPhone Personal Hotspot blocks/throttles client-to-client (LAN-internal) traffic** as an undocumented Apple security policy. NORA → cellular → Google STT works fine (outbound WAN path). NORA → iPhone → PC on the same hotspot is silently dropped.

Subnet `172.20.10.0/28` (mask `255.255.255.240`) is the Apple hotspot signature.

### Evidence

- STT call to `speech.googleapis.com` succeeds (~3 s end-to-end)
- LAN call to `172.20.10.3:5000` times out at 15 s
- Same NORA, same boot, same session
- Reproducible across 100% of `search_via_pc` calls on this network
- Earlier session on real Wi-Fi: PC search RTT ~100 ms

### Proposed fix

1. **Network change (real fix):** connect NORA and PC to a normal Wi-Fi router. LAN HTTP RTT drops from 15 s timeout to <100 ms.
2. **Defensive code (interim):** lower `esp_http_client` timeout from 15 s to 3 s so the user gets failure feedback faster. Also trigger `pc_disc` re-broadcast on failure in case the PC IP changed.

### Acceptance criteria

- [ ] `search_via_pc` succeeds in <500 ms on a healthy network
- [ ] On failure: user sees a UI error within 3 s, not 15 s
- [ ] Degenerate `TRACK:X||` is NOT sent on failure (see BUG-007)

---

## BUG-005 — Wake-word false triggers on music/ambient noise

**Severity:** **HIGH** — costs paid Google STT calls, wastes bandwidth, creates runaway pattern
**Status:** Open

### Symptom

After a legitimate "Hey Noa play X" command, the wake-word classifier fires repeatedly on noise or playback audio. Each false trigger uploads a 96 KB WAV to Google STT, which correctly returns `no transcript`.

Pattern: triggers get *closer together* over time (159 s → 38 s → 23 s → 9 s gap) when music is playing on speakers near the mic.

```
I (400595) MUSIC: Done — TRACK sent, THUMB SENT       ← real command
I (559455) NORA: AUDIO:FILE 'REC_459.wav' ... 96512 bytes
W (567145) cloud_upload: STT returned no transcript    ← false #1
I (597295) NORA: AUDIO:FILE 'REC_460.wav' ...
W (604905) cloud_upload: STT returned no transcript    ← false #2
I (620165) NORA: AUDIO:FILE 'REC_461.wav' ...
W (627855) cloud_upload: STT returned no transcript    ← false #3
I (629045) NORA: AUDIO:FILE 'REC_462.wav' ...
W (636895) cloud_upload: STT returned no transcript    ← false #4
```

### Root cause

Threshold (70 %) is too low for noisy / playback environments. No on-device audio-quality gate prevents uploading a noise-only WAV. No feedback loop from STT empty → wake-word backoff.

### Cost impact

- 1 paid Google STT call per false trigger
- 96 KB upload per false trigger
- 4 triggers in 70 s observed → could spike GCP bill significantly during music playback

### Proposed fix (three layers, do all)

**Layer 1 — On-device peak gate (highest leverage, zero recall cost).** In `voice_recorder.c`, after the 3-s capture completes, compute peak amplitude. If `peak < 2000`, discard and skip the UART send. Catches noise-only false triggers without ever hitting the cloud.

**Layer 2 — Raise wake-word threshold.** `WAKEWORD_THRESHOLD_PCT` from 70 → 80. Reduces false-trigger rate by ~50 %. Small recall cost (~5–10 %).

**Layer 3 — Adaptive backoff on consecutive empty transcripts.** NORA sends `STATUS:NO_TRANSCRIPT` to STM32. After 2 consecutive empties, raise threshold to 85 % and extend cooldown to 30 s. Reset on first successful transcript.

### Acceptance criteria

- [ ] Layer 1: a noise-only 3-s WAV produces `[REC] discard: peak=N below threshold` and no UART send
- [ ] Layer 2: real "Hey Noa" still triggers at ≥80 % live confidence
- [ ] Layer 3: simulate 2 consecutive empties → threshold auto-raises to 85 %
- [ ] 30-min idle-with-music soak: <5 false triggers total

### Related

- See BUG-006 for the speaker→mic bleed root cause when music is playing
- See BUG-009 for the audio-quality / SNR sub-problem

---

## BUG-006 — Speaker→mic acoustic bleed corrupts wake-word and commands

**Severity:** **HIGH** — system becomes unusable while music is playing
**Status:** Open

### Symptom

When a song is playing on the speakers near the STM32H747I-DISCO mic, two things break:

1. **False wake-word triggers** at high frequency (see BUG-005)
2. **Real commands get corrupted** — "Hey Noa play Queen" transcribed as "play machine" because the new command audio mixes with the currently-playing song

```
I (37985) cloud_upload: Transcript: "play Led Zeppelin"     ← real first command
I (37985) MUSIC: search_via_pc: ... → PLAYBACK STARTS
...
W (64285) cloud_upload: STT returned no transcript          ← false trigger from music
W (72015) cloud_upload: STT returned no transcript          ← another
```

### Root cause

The STM32H747I-DISCO has no acoustic echo cancellation (AEC). The mic picks up:
- The song's vocal/melodic content → scores as "speech-like" → wake-word triggers
- The song's audio overlapping with the user's actual voice command → STT misrecognises

This is the classic problem all consumer voice assistants (Alexa, Google Home, Siri) solve with **AEC** + **audio ducking**.

### Proposed fixes (ranked by effort/impact)

**Fix A — Wake-word suppression during playback (30 min, easy):**
When NORA sends `TRACK:...`, set `g_musicPlaying = true` on STM32. Wake-word classifier short-circuits while flag is set, until a `STOP` UART message arrives or N minutes pass.

Pros: very simple, eliminates false triggers during playback entirely.
Cons: user can't "barge in" — must wait for song to end or use button.

**Fix B — Audio ducking via PC (~2 h, medium):**
When wake word fires, STM32 sends `DUCK` over UART → NORA HTTP POST to PC `/volume?level=20` → PC reduces playback volume during the 3-s capture. After TRACK sent, restore volume.

Pros: industry-standard pattern, full voice control during playback.
Cons: **only works on a real Wi-Fi network** — see BUG-004. On iPhone hotspot, the duck command arrives after the capture is over (latency budget: ~150 ms on real Wi-Fi vs 1100–7300 ms on hotspot).

**Fix C — Acoustic Echo Cancellation (1–2 weeks, hard):**
Adaptive filter (NLMS) on STM32 that subtracts the speaker reference signal from the mic input. Proper solution but a research-level DSP project.

### Recommendation

**Ship Fix A first.** It works regardless of network and immediately removes the pain. Fix B is the right next step once BUG-004 is resolved.

### Acceptance criteria

- [ ] Fix A: during music playback, wake-word never fires regardless of song content
- [ ] Fix A: button trigger (PC13) still works during playback (separate gate)
- [ ] Fix B (after BUG-004): volume drops by ≥60 % within 200 ms of wake-word detection on a real Wi-Fi network

---

## BUG-007 — Degenerate TRACK message sent on PC-search failure

**Severity:** **MEDIUM** — misleading UX, user thinks song is loading when search actually failed
**Status:** Open

### Symptom

When `search_via_pc` fails (BUG-004), NORA still sends a TRACK message to STM32, but with empty videoId and thumbnail URL:

```
E (747215) MUSIC: search_via_pc: open failed: ESP_ERR_HTTP_CONNECT
I (747225) MUSIC: TRACK sent: TRACK:Led Zeppelin||             ← empty after second |
W (747225) MUSIC: THUMB NOT sent — found=0 url=(empty)
I (747235) MUSIC: Done — TRACK sent, THUMB NOT SENT
```

STM32 receives `TRACK:Led Zeppelin||`, displays "Led Zeppelin" as the title with empty artist and no thumbnail. User assumes the song is queued / loading.

### Root cause

NORA's MUSIC task unconditionally calls `send_track()` after the search attempt completes, regardless of success/failure status.

### Proposed fix

In NORA `music_task.c`:

```c
if (search_result.ok) {
    send_track_full(name, video_id, thumb_url);
} else {
    // Fix: explicit failure message
    send_uart("ERROR:search_failed\r\n");
    ESP_LOGW(TAG, "search failed — not sending degenerate TRACK");
}
```

Then STM32 LCD displays "Couldn't find that song" or similar instead of a half-loaded track.

### Acceptance criteria

- [ ] On search failure: no `TRACK:` message sent, `ERROR:search_failed` sent instead
- [ ] STM32 UI shows an explicit error state, not a broken-looking track
- [ ] Retry path: user can issue a new wake-word command immediately

---

## BUG-008 — Stale "BLE search mode" log string in NORA

**Severity:** **LOW** — cosmetic, but actively misleading during debugging
**Status:** Open

### Symptom

```
I (37985) MUSIC: BLE search mode: "Led Zeppelin"
```

The string says "BLE" but no BLE activity actually happens. The next step is a Wi-Fi HTTP call to the local PC.

### Root cause

BLE was permanently released in commit `2a37840` to free 71 KB of heap for the 96 KB WAV buffer (no PSRAM on the NORA-W106-00B variant). The log string was never updated when the search backend migrated from BLE to HTTP.

### Proposed fix

In NORA `music_task.c`:

```c
- ESP_LOGI(TAG, "BLE search mode: \"%s\"", song);
+ ESP_LOGI(TAG, "search via PC: \"%s\"", song);
```

One line, pure log-string hygiene.

### Acceptance criteria

- [ ] No log line in NORA references "BLE" unless BLE is actually active (which it shouldn't be)

---

## BUG-009 — STM32 DFSDM diagnostic block prints false [FAIL] tags

**Severity:** **LOW** — cosmetic, but wastes debugging time on every PING cycle
**Status:** Open

### Symptom

Every PING / Audio Quality Report block prints:

```
[CLK] SAI4PDMCR=00000000[FAIL] SAI4CR1=00000000 SAIEN=0[FAIL-PE2_FLAT] CKABF=FF[NO_CLK]
[BDMA] CCR=00000000[OFF-PE2_DEAD] CNDTR=0
[REG] DFSDM1_Ch1->CHCFGR1 = 0x00000000  (expect 0x00000085)
[REG] DFSDM1_Ch1->CHCFGR2 = 0x00000000  (expect 0x00000030)
[REG] DMA1_St1->M0AR      = 0x3001E000  (expect 0x30000000)
```

All four lines suggest a hardware failure. **The system is actually working correctly** — these checks haven't been updated since the architecture migrations.

### Root cause

Multiple stale diagnostic checks:

1. **SAI4 / BDMA checks:** Project migrated to **Path C-PC2** on 2026-04-28 — DFSDM Channel 0 generates the clock internally, SAI4 is intentionally gated off (`MX_SAI4_Init` wrapped in `#if 0`). The `[FAIL-PE2_FLAT]` / `[OFF-PE2_DEAD]` tags are checking a pipeline that no longer exists.

2. **CH1 register checks:** Active channel is now CH0, not CH1. Reading CH1 registers shows zeros and the diagnostic flags them as broken — but the data is actually flowing on CH0.

3. **DMA target address check:** `s_DfsdmBuf` was moved to `0x3001E000` during the Edge Impulse pool integration. The "expect 0x30000000" comparison is from the old layout.

### Proposed fix

In `voice_recorder.c`, either:

- **(a) Update checks** to reference CH0 and the current `s_DfsdmBuf` address
- **(b) Delete the SAI4/BDMA diagnostic block** entirely — they're no longer in the design

Recommend (b) for simplicity. The DFSDM CH0 register check (`CHCFGR1 = 0x80310085`) is the only one worth keeping, and it should compare against the CH0 expectation, not CH1.

### Acceptance criteria

- [ ] No `[FAIL]` / `[NO_CLK]` / `[PE2_DEAD]` strings appear in PING output of a healthy system
- [ ] Any remaining diagnostic checks compare against the current architecture (CH0, `0x3001E000`)
- [ ] An actual hardware failure (e.g. mic unplugged) still produces a useful error

---

## BUG-010 — Audio quality: SNR ~0 dB, DC offset 924, speech barely above noise

**Severity:** **MEDIUM** — root cause of many STT mis-transcriptions
**Status:** Open

### Symptom

```
[PCM] noise_rms=1136   speech_rms=1202   snr~+0 dB
[PCM] peak=4960        dc_offset=924     clip=0
[PCM] target : noise<300  speech>2000  snr>20dB
[PCM] FAIL: poor SNR -- check PDM filter config
```

Speech and noise are at the same RMS level. STT correctly transcribes loud clear utterances but mis-hears anything moderate (e.g. "Queen" → "machine", "Mashina" → "machine").

### Root cause

Three contributing factors:

1. **No DC blocker** — `dc_offset=924` is significant and shifts the full waveform off zero
2. **Mic placement / distance** — peak of 4960 is OK but noise floor of 1136 is much too high; speaker likely >30 cm from mic
3. **Software gain may need re-tuning** — `EI_PCM_GAIN=8` was set for an earlier mic configuration

### Proposed fix

**Layer 1 — DC blocker in `StoreDmaChunk` (5 minutes, biggest single win):**

```c
static int32_t prev_x = 0, prev_y = 0;
int32_t y = sample - prev_x + (prev_y * 31) / 32;  // 1-pole HPF at ~30 Hz
prev_x = sample;
prev_y = y;
return (int16_t)y;
```

Removes the 924-LSB DC offset, expected to raise speech_rms / lower effective noise_rms.

**Layer 2 — Re-tune mic gain after DC blocker is in:**
Re-run Audio Quality Report. If `speech_rms < 1500` after DC removal, raise `EI_PCM_GAIN` from 8 → 16. If `clip > 0`, lower it.

**Layer 3 — Document mic placement requirement** in user-facing instructions: "Speak within 30 cm of the board for best recognition."

### Acceptance criteria

- [ ] After DC blocker: `dc_offset < 100`
- [ ] After gain re-tune: `speech_rms > 2000`, `noise_rms < 500`, `snr > 12 dB`
- [ ] PASS line replaces FAIL in Audio Quality Report

---

## BUG-011 — Wake-word phrase bleeds into STT recording (wasted bytes)

**Severity:** **LOW** — works, just suboptimal
**Status:** Open (attempted fix reverted 2026-05-18)

### Symptom

STT transcripts include the wake-word phrase:

```
I (144945) cloud_upload: Transcript: "hey no I play mashina"
```

`cmd_router` Rule A1 correctly extracts `mashina` from this, so functionally OK — but every command pays Google STT to transcribe ~700 ms of wake-word audio that gets discarded.

### Root cause

The STM32 starts the 3-second capture at the moment of wake-word detection. The user's "Hey Noa" phrase sits in the first ~700 ms of the WAV.

### Status: NOT FIXING

Attempted fix on 2026-05-18 (`#ifdef WAKEWORD_SKIP_BLEED_MS 600u`) was reverted. The `cmd_router` parser is robust enough to handle the bleed-through, so the added firmware complexity (queue-drain loop, dma_q_ovf risk, edge cases when users speak fast) wasn't worth the ~20 % byte saving. **Smart parser > clever firmware** on this trade-off.

### When to reconsider

- If Google STT costs become a real line item (e.g. >$100/month)
- If a future, stricter parser fails on bled-through transcripts
- If switching to inline-audio STT API (saves 1 TLS handshake per call) and want to also save bandwidth

The reverted patch is in git history — resurrect the diff if needed.

---

## BUG-012 — Two TLS handshakes per voice command (300 ms latency)

**Severity:** **LOW** — optimisation opportunity, not a defect
**Status:** Open / Won't fix yet

### Symptom

Every voice command does two HTTPS handshakes:

```
26895  esp-x509-crt-bundle: Certificate validated   ← GCS upload cert
29455  esp-x509-crt-bundle: Certificate validated   ← STT API cert
```

Each TLS handshake on the ESP32 is ~250–350 ms (RTT + RSA verify + key exchange). Two handshakes = ~600 ms of pure handshake overhead per voice command.

### Root cause

Current architecture is "upload WAV to GCS, then call STT with the GCS URI". Two different Google services = two different hosts = two TLS sessions that can't be reused.

### Proposed fix

Switch to **Google STT's `recognize` API with inline audio** instead of `audio.uri`:

```json
POST speech.googleapis.com/v1/speech:recognize
{
  "config": { "encoding": "LINEAR16", "sampleRateHertz": 16000, "languageCode": "en-US" },
  "audio": { "content": "<base64-of-96KB-WAV>" }
}
```

Effects:
- One TLS handshake instead of two → ~300 ms shorter end-to-end latency
- No more "Certificate validated" pair in log
- Slightly lower GCP bill (no GCS storage operations)
- Trade-off: WAV bytes base64-encoded inline → ~130 KB request body (33% more bytes than 96 KB raw)

### Effort estimate

2–3 hours (see conversation notes 2026-05-18). Main considerations: heap budget for the 130 KB encoded buffer (NORA tight after BLE removal) and base64 streaming if needed.

### Acceptance criteria

- [ ] Only one "Certificate validated" line per voice command
- [ ] End-to-end latency reduced by ~300 ms
- [ ] Heap free remains > 50 KB at peak (during base64 encode)

---

## BUG-013 — TLS heap exhaustion on overlapping voice commands

**Severity:** **MEDIUM** — recoverable on next command, but breaks the *currently in-flight* one
**Status:** Open / known workaround = "wait > 15 s between commands"

### Symptom

When a second wake-word fires before the previous command's ntfy.sh `/res` poll has completed, NORA's TLS handshake for the new GCS upload fails with:

```
E (374307) esp-x509-crt-bundle: PK verify failed with error 0x4290
E (374307) esp-x509-crt-bundle: Certificate matched but signature verification failed
E (374317) esp-tls-mbedtls: mbedtls_ssl_setup returned -0x7F00       ← MBEDTLS_ERR_SSL_ALLOC_FAILED
E (374317) esp-tls: create_ssl_handle failed
E (374317) esp-tls: Failed to open new connection
... (25× repeats over ~13 s) ...
```

The new command's STT request never completes; user perceives it as "the second one didn't work."

### Root cause

ESP32-S3 has ~520 KB RAM total. mbedTLS needs ~30–50 KB **per active session**. Normal flow uses 1 TLS session at a time, but during the ntfy `/res` poll-loop (running every ~250 ms for up to 15 s after a command), the *next* command's GCS upload opens a **second** concurrent TLS session. Heap allocator can't satisfy the second alloc → handshake fails.

First observed 2026-05-23 with the ntfy.sh relay added in `f646e3b`. The new 2-call sequential STT (added today, [`db87b00`+ in cloud_upload.c](../NORA_BLE/nora_wifi_ble_translate/main/cloud_upload.c)) does **not** make this worse — its two calls are sequential, never concurrent.

### Workaround (today)

Wait ~10–15 s between voice commands. The bug doesn't manifest at normal usage speeds.

### Proposed fixes (two options)

**Option 1 — STM32 BUSY/READY gating.** NORA sends `BUSY` over UART when it enters the ntfy poll loop, `READY` when the poll completes. STM32's wake-word detector ignores triggers while NORA is `BUSY`.

| Pro | Con |
|-----|-----|
| Solves the heap conflict at the architectural level | New UART message types + STM32 state machine change |
| Naturally serialises end-to-end | User has to wait — but they'd wait anyway |
| Tiny impact on per-command latency | Adds a small "system busy" surface to debug if STM32 gets stuck |

**Option 2 — Single long-lived ntfy poll connection.** Replace the `?since=20s&poll=1` short-poll loop (new TLS session every 250 ms) with a single long-lived SSE/streaming GET that stays open for the full 15 s timeout window.

| Pro | Con |
|-----|-----|
| Reduces total TLS-handshake count from ~60 → 1 per command | ESP-IDF HTTP client streaming API has rougher edges than the simple POST/GET we use today |
| Cuts per-poll latency (no handshake) → first response ~250 ms faster | Long-lived TCP needs keepalive + reconnect handling |
| Doesn't require STM32 changes | More complex than Option 1 |

**Recommendation:** Option 1 first (fewer moving parts, smaller surface area). Option 2 later as an optimisation if you want sub-300 ms search response times.

### Effort estimate

- Option 1: ~1 hour (1 UART command type each direction, ~30 lines on each side).
- Option 2: ~3–4 hours (rework `ntfy_client.c` to use chunked-streaming GET; verify with overlap-test).

### Acceptance criteria

- [ ] Speak commands 5 s apart × 3 times in a row — no `mbedtls_ssl_setup returned -0x7F00` lines in NORA log
- [ ] Heap free reported by NORA stays > 60 KB throughout the test
- [ ] No regression in single-command flow

---

## Priority matrix

| Bug | Severity | Effort | Priority |
|---|---|---|---|
| BUG-004 (iPhone hotspot blocks LAN) | HIGH | 5 min (plug in router) | **P0** |
| BUG-005 (false triggers on noise) | HIGH | 30 min (peak gate) | **P0** |
| BUG-006 (speaker→mic bleed) | HIGH | 30 min (Fix A) | **P0** |
| BUG-007 (degenerate TRACK on failure) | MEDIUM | 20 min | P1 |
| BUG-010 (SNR ~0 dB, DC offset) | MEDIUM | 30 min (DC blocker) | P1 |
| BUG-008 (BLE log string) | LOW | 1 min | P2 |
| BUG-009 (stale [FAIL] diagnostics) | LOW | 15 min | P2 |
| BUG-011 (wake-word bleed in STT) | LOW | already reverted | P3 / won't fix |
| BUG-012 (two TLS handshakes) | LOW | 2–3 hours | P3 / optimisation |
| BUG-013 (TLS heap exhaustion on overlap) | MEDIUM | 1 hour (Opt 1) / 3–4 hours (Opt 2) | **P1** |

**P0 = fix this week. P1 = fix next sprint. P2 = cleanup. P3 = nice-to-have.**

The single highest-leverage action is **BUG-004 (switch off iPhone hotspot)** — it unblocks the entire pipeline and is a 5-minute environmental change with no code involved.
