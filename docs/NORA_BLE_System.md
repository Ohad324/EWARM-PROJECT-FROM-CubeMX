wht # NORA BLE System — Documentation

**Project:** MyApplication (STM32H747I-DISCO + ESP32-S3 NORA)
**Date:** 2026-03-29

---

## 1. System Overview

The NORA BLE system connects an iPhone to a TouchGFX display via two wireless hops:

```
iPhone
  │  BLE (Nordic UART Service)
  ▼
ESP32-S3 (u-blox EVK-NORA-W106)
  │  UART — 921600 baud
  ▼
STM32H747I-DISCO (CM7 core)
  │  FreeRTOS tasks
  ▼
TouchGFX Display
```

The ESP32 also connects to Wi-Fi to perform Google Translate lookups and YouTube searches, and posts results back to the STM32 over UART.

---

## 2. Hardware

| Component | Role |
|-----------|------|
| STM32H747I-DISCO | Dual-core (CM7 + CM4). CM7 runs the TouchGFX application. CM4 boots via HSEM. |
| u-blox EVK-NORA-W106 (ESP32-S3) | BLE peripheral + Wi-Fi client. Bridges iPhone commands to the STM32. |
| UART8 (STM32) ↔ UART1 (ESP32) | 921600 baud, 8N1, no flow control. TX=GPIO17, RX=GPIO18 on ESP32. |

---

## 3. Repository Structure

```
NORA_BLE/
  nora_ble_bridge/               Simple BLE→UART passthrough (not used)
  nora_wifi_ble_translate/       Active firmware — BLE + Wi-Fi + Translate + Music
    main/
      nora_ble_bridge.c          BLE (NUS), Wi-Fi, Google Translate, task routing
      music_task.c               YouTube search, thumbnail stream, PC player POST
      music_task.h
      nora_ble_bridge.h

CM7/Core/Src/
  ble_uart.c                     DMA + IDLE-line UART8 driver (STM32 side)
  main.c                         UARTReceiveTask, routing, FreeRTOS setup

CM7/Core/Inc/
  ble_uart.h                     Driver API, BleRawMsg_t, bleHistory
  ble_queue.h                    xBleQueue shared between main.c and Model.cpp

tools/
  youtube_player.py              PC HTTP server — opens YouTube in Chrome on PLAY command
```

---

## 4. Communication Protocol (ESP32 → STM32 over UART)

All messages are ASCII lines terminated with `\n`, except thumbnail binary data.

| Message | Direction | Meaning |
|---------|-----------|---------|
| `RESULT:<hebrew text>\n` | ESP32 → STM32 | Translation result — forwarded to `xBleQueue` → TouchGFX |
| `TRACK:<title>\|<channel>\|<videoId>\n` | ESP32 → STM32 | Song metadata for music screen |
| `THUMB:<size>\n` + `<size> bytes` | ESP32 → STM32 | JPEG thumbnail binary stream (512-byte chunks) |
| `ERROR:<reason>\n` | ESP32 → STM32 | Error notification |
| `READY\n` | ESP32 → STM32 | Wi-Fi connected and ready |
| `TRANSLATE:<langpair>:<word>\n` | STM32 → ESP32 | STM32-initiated translation request |

---

## 5. BLE Interface (iPhone → ESP32)

Uses the **Nordic UART Service (NUS)**:

- **Service UUID:** `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- **RX characteristic** (`...0002`): Write from phone — receives commands
- **TX characteristic** (`...0003`): Notify to phone — sends back YouTube URL

### Commands from iPhone

| Command | Action |
|---------|--------|
| `hello` or any word | Translate with default language pair (en→he) |
| `en\|fr:hello` | Translate with specified language pair |
| `PLAY:<search query>` | Search YouTube, stream thumbnail to STM32, open video on PC |

---

## 6. STM32 BLE Stack (CM7)

### DMA + IDLE-line UART driver (`ble_uart.c`)

- **DMA1 Stream0**, DMAMUX1 request `UART8_RX`
- Buffer: `s_dma_rx_buf[128]` — 32-byte aligned, placed in AXI SRAM (DMA-accessible)
- D-Cache invalidated via `SCB_InvalidateDCache_by_Addr()` before CPU reads
- On IDLE interrupt: copies buffer → `BleRawMsg_t`, posts to `xRawBleQueue`, restarts DMA immediately

### FreeRTOS objects

| Object | Type | Purpose |
|--------|------|---------|
| `xRawBleQueue` | Queue (4 slots) | ISR → `UARTReceiveTask` raw DMA snapshots |
| `xBleQueue` | Queue (8 slots, 64 B each) | `UARTReceiveTask` → `Model::tick()` RESULT messages |
| `xBleHistMutex` | Mutex | Protects `bleHistory[10][32]` rolling log |

### `UARTReceiveTask`

- Priority: `osPriorityAboveNormal` (above TouchGFX)
- Stack: 4 KB
- Accumulates DMA bursts into a 512-byte buffer, routes complete lines by prefix:
  - `RESULT:` → `xBleQueue`
  - `TRACK:` → `xMusicQueue`
  - `THUMB:` → binary receive mode
  - `ERROR:` → `xMusicQueue`

---

## 7. Music Feature Flow

```
iPhone sends "PLAY:hey jude"
  │
  ▼ BLE (NUS RX write)
ESP32 music_task
  ├─ YouTube Data API v3 search → videoId, title, channel, thumbnailUrl
  ├─ BLE notify → iPhone  ("https://youtu.be/<id>")
  ├─ HTTP POST → PC player  (nora-player.local:5000/play)
  ├─ UART → STM32  TRACK:<title>|<channel>|<videoId>\n
  └─ UART → STM32  THUMB:<size>\n + JPEG bytes (512 B chunks)
```

### PC Player (`tools/youtube_player.py`)

- HTTP server on port 5000, endpoint `POST /play`
- Accepts `{"videoId": "...", "title": "...", "artist": "..."}`
- Opens YouTube in Chrome (falls back to default browser)
- Downloads best available thumbnail (maxres → hq → mq) in background thread and opens in image viewer
- Advertises itself as **`nora-player.local`** via mDNS (`zeroconf`) — no hardcoded IP needed
- **Auto-starts at Windows login** via startup folder: `%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup\nora_youtube_player.bat`
  - Restarts automatically if it crashes (3-second retry loop)

---

## 8. Setup & Dependencies

### ESP32 firmware
- ESP-IDF project: `NORA_BLE/nora_wifi_ble_translate/`
- Build and flash with `idf.py build flash`
- Wi-Fi credentials: hardcoded in `nora_ble_bridge.c` (`WIFI_SSID` / `WIFI_PASSWORD`) — update before flashing for a new network
- YouTube API key: hardcoded in `music_task.c` (`YOUTUBE_API_KEY`)

### PC player
```
pip install zeroconf
python tools/youtube_player.py
```
Runs automatically at login — no manual start needed.

### IAR project
- Location: `EWARM/STM32H747I-DISCO.ewp`
- Board: STM32H747I-DISCO
- Active core: CM7

---

## 9. Known Limitations

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| Wi-Fi SSID/password hardcoded | Must reflash ESP32 when changing networks | Add second SSID entry when office credentials are available |
| YouTube API key hardcoded | Key exposed in source | Keep repo private; rotate key if leaked |
| Music thumbnail: maxresdefault only | Low-res thumbnails skipped entirely | By design — ensures display quality |

---

## 10. Bugs Fixed (2026-03-29)

### Race condition in `nora_wifi_ble_translate` (`nora_ble_bridge.c`)
**Problem:** `s_ble_queue` was created *after* `nimble_port_freertos_init()`. If a phone auto-reconnected and wrote a BLE characteristic before `app_main` reached the `xQueueCreate` call, `nus_rx_access` would call `xQueueSend(NULL, ...)` → assert/crash.

**Fix:** Moved `xQueueCreate` to before `nimble_port_freertos_init()`.

### Hardcoded PC IP address (`music_task.c`)
**Problem:** `PC_PLAYER_URL` was hardcoded to `http://10.100.102.2:5000/play`. The system would silently fail to open YouTube on the PC whenever the PC's IP changed (different network, DHCP reassignment).

**Fix:** Changed URL to `http://nora-player.local:5000/play`. The PC now advertises itself as `nora-player.local` via mDNS (`zeroconf`). The ESP32 resolves the hostname dynamically via lwIP (`CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`). Works on any network without reconfiguration.
