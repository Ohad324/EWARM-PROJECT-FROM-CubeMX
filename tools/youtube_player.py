"""
youtube_player.py — HTTP receiver for NORA music playback commands.

Two entry points:
  1. POST /play    — sent by NORA (ESP32) when a track is identified via YouTube API.
                     Opens Chrome with the video and downloads the thumbnail.
  2. POST /command — sent by NORA for raw voice commands (play/stop/pause/next/previous/volume).
                     Controls the browser via keyboard media keys (requires pyautogui).

NORA (ESP32) /play example:
    POST http://nora-player.local:5000/play
    Content-Type: application/json
    {"videoId": "dQw4w9WgXcQ", "title": "Hey Jude", "artist": "Beatles"}

NORA /command example:
    POST http://nora-player.local:5000/command
    Content-Type: application/json
    {"command": "play hey jude"}

This script advertises itself as nora-player.local via mDNS so NORA can
find it on any network without a hardcoded IP.

Usage:
    pip install zeroconf
    python youtube_player.py

Optional env vars:
    PORT=5000          (default 5000)
    BROWSER=chrome     (chrome | default)  — "default" uses the OS default browser
"""

import os
import sys
import json
import webbrowser
import subprocess
import platform
import tempfile
import threading
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

# Optional: pyautogui is used for media key presses (pause, next, volume).
# If not installed, those voice commands are silently ignored.
try:
    import pyautogui
    _PYAUTOGUI_OK = True
except ImportError:
    _PYAUTOGUI_OK = False

# Optional: zeroconf advertises this PC as nora-player.local on the LAN.
# If not installed, NORA must use a hardcoded IP instead of the hostname.
try:
    from zeroconf import Zeroconf, ServiceInfo
    _ZEROCONF_OK = True
except ImportError:
    _ZEROCONF_OK = False

# ── Configuration ────────────────────────────────────────────────────────────

PORT    = int(os.environ.get("PORT", 5000))
BROWSER = os.environ.get("BROWSER", "chrome").lower()   # "chrome" or "default"

# Dedicated Chrome profile for NORA's YouTube window.
# Using a separate --user-data-dir makes Chrome open as an independent, trackable
# process — so we can close exactly this window when a new song is requested,
# without affecting the user's regular Chrome session.
CHROME_PROFILE_DIR = os.path.join(os.environ.get("APPDATA", tempfile.gettempdir()), "nora_chrome_profile")

YOUTUBE_BASE   = "https://www.youtube.com/watch?v="  # append videoId + &autoplay=1
YOUTUBE_SEARCH = "https://www.youtube.com/results?search_query={query}"

MEDIA_KEYWORDS   = ["play", "stop", "pause", "next", "previous", "volume"]

# Thumbnail quality candidates — tried in order, first successful one wins.
# YouTube may return a tiny placeholder for maxresdefault if unavailable,
# so we check the file size and fall back to lower quality if too small.
THUMB_URLS = [
    "https://img.youtube.com/vi/{id}/maxresdefault.jpg",   # 1280x720 (best)
    "https://img.youtube.com/vi/{id}/hqdefault.jpg",       # 480x360
    "https://img.youtube.com/vi/{id}/mqdefault.jpg",       # 320x180 (fallback)
]

# Always saved to the same temp file so each new song overwrites the previous one.
THUMB_PATH = os.path.join(tempfile.gettempdir(), "nora_thumbnail.jpg")

# ── YouTube search (no API key) ───────────────────────────────────────────────

import re

def _youtube_search_first_id(query: str) -> str | None:
    """Search YouTube and return the first video ID — no API key required.
    Fetches the search results page and extracts the first videoId from the
    embedded JSON data that YouTube includes in every search page."""
    try:
        search_url = f"https://www.youtube.com/results?search_query={urllib.parse.quote(query)}"
        req = urllib.request.Request(search_url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=5) as r:
            html = r.read().decode("utf-8")
        match = re.search(r'"videoId":"([a-zA-Z0-9_-]{11})"', html)
        if match:
            video_id = match.group(1)
            print(f"[search] Resolved \"{query}\" -> videoId={video_id}")
            return video_id
    except Exception as e:
        print(f"[search] Failed to resolve \"{query}\": {e}")
    return None


# ── Thumbnail downloader ──────────────────────────────────────────────────────

def _fetch_and_open_thumbnail(video_id: str) -> None:
    """Download the best available thumbnail and open it in the OS image viewer.
    Runs in a background thread so it doesn't block the HTTP response."""
    print(f"[thumb]  Fetching thumbnail for videoId={video_id} ...")
    for url_tpl in THUMB_URLS:
        url = url_tpl.format(id=video_id)
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                data = resp.read()
            # YouTube returns a 120x90 grey placeholder for missing maxresdefault.
            # Skip it if the file is suspiciously small (< 5 KB).
            if len(data) < 5000:
                print(f"[thumb]  {url_tpl.split('/')[-1]} too small ({len(data)} B), trying next...")
                continue
            with open(THUMB_PATH, "wb") as f:
                f.write(data)
            print(f"[thumb]  Saved: {THUMB_PATH}  ({len(data):,} bytes)")
            return
        except Exception as e:
            print(f"[thumb]  {url_tpl.split('/')[-1]} failed: {e}")
    print("[thumb]  ERROR: no thumbnail found for this videoId.")


def _open_file(path: str) -> None:
    """Open a file with the OS default application and confirm."""
    system = platform.system()
    try:
        if system == "Windows":
            # Use 'start' via cmd — more reliable than os.startfile on all Windows versions
            subprocess.Popen(f'cmd /c start "" "{path}"', shell=True)
        elif system == "Darwin":
            subprocess.Popen(["open", path])
        else:
            subprocess.Popen(["xdg-open", path])
        print(f"[thumb]  Opened in default image viewer  OK")
    except Exception as e:
        print(f"[thumb]  ERROR: could not open file: {e}")


# ── Browser launcher ─────────────────────────────────────────────────────────

_chrome_proc = None   # tracks the currently open Chrome window so we can close it on the next song
_CHROME_PID_FILE = os.path.join(tempfile.gettempdir(), "nora_chrome.pid")

def _ensure_youtube_audio_allowed() -> None:
    """Write a Chrome Preferences file that grants YouTube sound permission to the
    NORA profile before the first launch. This bypasses the Media Engagement Index
    (MEI) requirement — a fresh profile has MEI=0 and Chrome mutes autoplay audio.
    Setting content_settings.exceptions.sound = 1 (Allow) for youtube.com is
    equivalent to the user manually choosing 'Allow' in Chrome site settings."""
    import json
    prefs_dir = os.path.join(CHROME_PROFILE_DIR, "Default")
    prefs_path = os.path.join(prefs_dir, "Preferences")
    os.makedirs(prefs_dir, exist_ok=True)

    # Read existing Preferences if Chrome already created them; otherwise start fresh.
    prefs = {}
    if os.path.exists(prefs_path):
        try:
            with open(prefs_path, "r", encoding="utf-8") as f:
                prefs = json.load(f)
        except Exception:
            prefs = {}

    # Drill down to content_settings → exceptions → sound and set YouTube = Allow (1).
    cs = prefs.setdefault("profile", {}) \
               .setdefault("content_settings", {}) \
               .setdefault("exceptions", {}) \
               .setdefault("sound", {})
    cs["https://www.youtube.com:443,*"] = {
        "expiration": "0",
        "last_modified": "13000000000000000",
        "model": 0,
        "setting": 1   # 1 = Allow, 2 = Block
    }

    with open(prefs_path, "w", encoding="utf-8") as f:
        json.dump(prefs, f)
    print("[player] YouTube audio permission written to NORA Chrome profile")


def _find_chrome() -> str | None:
    """Return path to chrome.exe on Windows, or None."""
    candidates = [
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
    ]
    return next((p for p in candidates if os.path.exists(p)), None)


def open_youtube_search(url: str) -> None:
    """Open a YouTube search URL in Chrome (or default browser if Chrome not found).
    Used when NORA sends a voice command with no resolved videoId."""
    chrome = _find_chrome()
    if chrome:
        subprocess.Popen([chrome, "--new-window", url],
                         creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    else:
        webbrowser.open(url)


def open_youtube(video_id: str, title: str, artist: str) -> None:
    global _chrome_proc
    url = YOUTUBE_BASE + video_id + "&autoplay=1"
    print(f"[player] Opening: {title} — {artist}")
    print(f"[player] URL: {url}")

    if BROWSER == "default":
        webbrowser.open(url)
        return

    chrome = _find_chrome()
    if not chrome:
        print("[player] Chrome not found, falling back to default browser")
        webbrowser.open(url)
        return

    # Close the previous NORA YouTube window if still open.
    # _chrome_proc covers the current session; the PID file covers across Python restarts
    # (e.g. bat kills and restarts Python between songs).
    import time
    prev_pid = None
    if _chrome_proc is not None and _chrome_proc.poll() is None:
        prev_pid = _chrome_proc.pid
    elif os.path.exists(_CHROME_PID_FILE):
        try:
            with open(_CHROME_PID_FILE) as f:
                prev_pid = int(f.read().strip())
        except Exception:
            prev_pid = None

    if prev_pid is not None:
        try:
            import signal, ctypes
            handle = ctypes.windll.kernel32.OpenProcess(1, False, prev_pid)
            if handle:
                ctypes.windll.kernel32.TerminateProcess(handle, 0)
                ctypes.windll.kernel32.CloseHandle(handle)
                print(f"[player] Previous YouTube window closed (pid={prev_pid})")
        except Exception as e:
            print(f"[player] Could not close previous window: {e}")
        time.sleep(0.5)

    _chrome_proc = None
    try:
        os.remove(_CHROME_PID_FILE)
    except Exception:
        pass

    # Grant YouTube audio permission to the NORA profile before launching.
    # Must be called every time — Chrome may overwrite Preferences on shutdown.
    _ensure_youtube_audio_allowed()

    # Open a dedicated Chrome instance using a separate profile directory.
    # --user-data-dir forces Chrome to start as a new independent process
    # instead of reusing the existing Chrome window, making it trackable
    # so we can close it when the next song is requested.
    _chrome_proc = subprocess.Popen(
        [chrome, f"--user-data-dir={CHROME_PROFILE_DIR}", url],
        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP
    )
    # Save PID to file so the next session can close this window even after Python restarts
    try:
        with open(_CHROME_PID_FILE, "w") as f:
            f.write(str(_chrome_proc.pid))
    except Exception:
        pass
    print(f"[player] Chrome opened (pid={_chrome_proc.pid})")


# ── mDNS advertisement ───────────────────────────────────────────────────────

def register_mdns(local_ip: str, port: int):
    """Advertise this PC as nora-player.local on the LAN via mDNS (zeroconf).
    Allows NORA to find the PC by hostname instead of a hardcoded IP.
    Returns the Zeroconf instance (needed to unregister on shutdown), or None on failure."""
    if not _ZEROCONF_OK:
        print("[mdns]  zeroconf not installed — run: pip install zeroconf")
        print("[mdns]  NORA will not be able to find this PC by hostname.")
        return None
    try:
        import socket as _socket
        zc = Zeroconf()
        info = ServiceInfo(
            "_http._tcp.local.",
            "nora-player._http._tcp.local.",
            addresses=[_socket.inet_aton(local_ip)],
            port=port,
            server="nora-player.local.",
        )
        zc.register_service(info)
        print(f"[mdns]  Registered as nora-player.local → {local_ip}:{port}")
        return zc
    except Exception as e:
        print(f"[mdns]  Registration failed: {e}")
        return None


# ── HTTP handler ─────────────────────────────────────────────────────────────

class Handler(BaseHTTPRequestHandler):

    def do_GET(self):
        """Serve the cached thumbnail file for NORA to fetch over local HTTP."""
        if self.path != "/thumbnail":
            self.send_response(404)
            self.end_headers()
            return
        if not os.path.exists(THUMB_PATH):
            self.send_response(404)
            self.end_headers()
            return
        with open(THUMB_PATH, "rb") as f:
            data = f.read()
        self.send_response(200)
        self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)
        print(f"[thumb]  Served {len(data):,} B to NORA via /thumbnail")

    def do_POST(self):
        """Route incoming POST requests to the correct handler."""
        if self.path == "/command":
            # Voice command from NORA (play/stop/pause/next/previous/volume)
            self._handle_voice_command()
            return
        if self.path != "/play":
            self._respond(404, {"error": "not found"})
            return

        length = int(self.headers.get("Content-Length", 0))
        body   = self.rfile.read(length)

        try:
            data = json.loads(body)
        except json.JSONDecodeError as e:
            print(f"[player] Bad JSON: {e}")
            self._respond(400, {"error": "invalid JSON"})
            return

        video_id = data.get("videoId", "").strip()
        query    = data.get("query",   "").strip()
        title    = data.get("title",   "Unknown").strip()
        artist   = data.get("artist",  "Unknown").strip()

        if not video_id and not query:
            self._respond(400, {"error": "missing videoId or query"})
            return

        if not video_id:
            # No videoId from NORA — search YouTube ourselves and open the first result directly
            video_id = _youtube_search_first_id(query)
            if not video_id:
                # Scrape failed — open search results page but return no videoId.
                # NORA will not send a THUMB, LCD keeps its current image.
                print(f"[player] Scrape failed for \"{query}\" — no videoId resolved")
                url = YOUTUBE_SEARCH.format(query=urllib.parse.quote(query))
                open_youtube_search(url)
                self._respond(200, {"status": "ok", "query": query})
                return
            title = title if title != "Unknown" else query

        open_youtube(video_id, title, artist)
        # Download thumbnail synchronously so /thumbnail is ready when NORA fetches it
        _fetch_and_open_thumbnail(video_id)
        self._respond(200, {"status": "ok", "videoId": video_id})

    def _handle_voice_command(self):
        """Handle POST /command — raw voice transcript from NORA.
        Supported: play <query>, stop, pause, next, previous, volume up/down.
        Media key presses require pyautogui to be installed."""
        length = int(self.headers.get("Content-Length", 0))
        body   = self.rfile.read(length)
        try:
            data = json.loads(body)
        except json.JSONDecodeError as e:
            self._respond(400, {"error": f"invalid JSON: {e}"})
            return

        command = data.get("command", "").strip().lower()
        if not command:
            self._respond(400, {"error": "missing command"})
            return

        print(f"[voice]  Command received: \"{command}\"")

        if "play" in command:
            # Extract search query by stripping the word "play"
            query = command.replace("play", "").strip() or "music"
            url   = YOUTUBE_SEARCH.format(query=urllib.parse.quote(query))
            print(f"[voice]  → YouTube search: \"{query}\"")
            open_youtube_search(url)
            self._respond(200, {"status": "ok", "action": "play", "query": query})

        elif "stop" in command or "pause" in command:
            print("[voice]  → Pause/stop")
            if _PYAUTOGUI_OK:
                pyautogui.press("space")   # spacebar toggles play/pause in YouTube
            self._respond(200, {"status": "ok", "action": "pause"})

        elif "next" in command:
            print("[voice]  → Next track")
            if _PYAUTOGUI_OK:
                pyautogui.press("nexttrack")
            self._respond(200, {"status": "ok", "action": "next"})

        elif "previous" in command or "prev" in command:
            print("[voice]  → Previous track")
            if _PYAUTOGUI_OK:
                pyautogui.press("prevtrack")
            self._respond(200, {"status": "ok", "action": "previous"})

        elif "volume" in command:
            if "down" in command or "lower" in command:
                print("[voice]  → Volume down")
                if _PYAUTOGUI_OK:
                    for _ in range(5): pyautogui.press("volumedown")
                self._respond(200, {"status": "ok", "action": "volume_down"})
            else:
                print("[voice]  → Volume up")
                if _PYAUTOGUI_OK:
                    for _ in range(5): pyautogui.press("volumeup")
                self._respond(200, {"status": "ok", "action": "volume_up"})
        else:
            print(f"[voice]  → Unknown command: \"{command}\"")
            self._respond(200, {"status": "unknown", "command": command})

    def _respond(self, code: int, body: dict) -> None:
        """Send a JSON HTTP response."""
        payload = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        """Override default HTTP log format to match our [tag] style."""
        print(f"[http]  {self.address_string()} — {fmt % args}")


# ── Entry point ───────────────────────────────────────────────────────────────

# ── UDP discovery responder ───────────────────────────────────────────────────
# NORA broadcasts "NORA_HELLO" on UDP port 5001 at boot; we reply with our IP.
DISCOVERY_PORT = 5001

def discovery_responder(local_ip: str, http_port: int):
    """Background thread: reply to NORA's UDP broadcast with our IP+port."""
    import socket, threading
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("0.0.0.0", DISCOVERY_PORT))
    print(f"[disc]   UDP discovery listening on :{DISCOVERY_PORT}")
    while True:
        try:
            data, addr = sock.recvfrom(64)
            msg = data.decode("ascii", errors="replace").strip()
            if msg == "NORA_HELLO":
                reply = f"PLAYER:{local_ip}:{http_port}".encode("ascii")
                sock.sendto(reply, addr)
                print(f"[disc]   NORA_HELLO from {addr[0]} → replied {reply.decode()}")
        except Exception as e:
            print(f"[disc]   error: {e}")


if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", PORT), Handler)

    # Print the local IP so NORA knows where to POST
    import socket, threading
    # Detect local IP by briefly connecting to an external address (no data is sent)
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except OSError:
        local_ip = "127.0.0.1"

    # Advertise as nora-player.local so NORA can find us without a hardcoded IP
    zeroconf = register_mdns(local_ip, PORT)

    # Start UDP discovery responder in a daemon thread
    threading.Thread(
        target=discovery_responder,
        args=(local_ip, PORT),
        daemon=True,
    ).start()

    print(f"[player] Listening on http://0.0.0.0:{PORT}/play")
    print(f"[player] mDNS hostname:  http://nora-player.local:{PORT}/play")
    print(f"[player] Local IP:       http://{local_ip}:{PORT}/play")
    print(f"[player] Browser mode:   {BROWSER}")
    print(f"[player] Press Ctrl+C to stop\n")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[player] Stopped.")
        server.server_close()
        # Clean up mDNS so the hostname is released on the network
        if zeroconf:
            zeroconf.unregister_all_services()
            zeroconf.close()
