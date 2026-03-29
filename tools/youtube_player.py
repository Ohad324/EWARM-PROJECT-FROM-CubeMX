"""
youtube_player.py — HTTP receiver for NORA music playback commands.

NORA (ESP32) sends:
    POST http://nora-player.local:5000/play
    Content-Type: application/json
    {"videoId": "dQw4w9WgXcQ", "title": "Hey Jude", "artist": "Beatles"}

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
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

try:
    from zeroconf import Zeroconf, ServiceInfo
    _ZEROCONF_OK = True
except ImportError:
    _ZEROCONF_OK = False

# ── Configuration ────────────────────────────────────────────────────────────

PORT    = int(os.environ.get("PORT", 5000))
BROWSER = os.environ.get("BROWSER", "chrome").lower()   # "chrome" or "default"

YOUTUBE_BASE = "https://www.youtube.com/watch?v="

# Thumbnail candidates — tried in order, first successful one wins
THUMB_URLS = [
    "https://img.youtube.com/vi/{id}/maxresdefault.jpg",
    "https://img.youtube.com/vi/{id}/hqdefault.jpg",
    "https://img.youtube.com/vi/{id}/mqdefault.jpg",
]

# Fixed path so each new song overwrites the previous thumbnail
THUMB_PATH = os.path.join(tempfile.gettempdir(), "nora_thumbnail.jpg")

# ── Thumbnail downloader ──────────────────────────────────────────────────────

def _fetch_and_open_thumbnail(video_id: str) -> None:
    """Download the best available thumbnail and open it (runs in background)."""
    print(f"[thumb]  Fetching thumbnail for videoId={video_id} ...")
    for url_tpl in THUMB_URLS:
        url = url_tpl.format(id=video_id)
        try:
            with urllib.request.urlopen(url, timeout=5) as resp:
                data = resp.read()
            # YouTube returns a 120x90 "no thumbnail" placeholder for missing
            # maxresdefault — skip it if the file is suspiciously small.
            if len(data) < 5000:
                print(f"[thumb]  {url_tpl.split('/')[-1]} too small ({len(data)} B), trying next...")
                continue
            with open(THUMB_PATH, "wb") as f:
                f.write(data)
            print(f"[thumb]  Saved: {THUMB_PATH}  ({len(data):,} bytes)")
            _open_file(THUMB_PATH)
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

_chrome_proc = None   # track the Chrome app window we launched

def _find_chrome() -> str | None:
    """Return path to chrome.exe on Windows, or None."""
    candidates = [
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Google\Chrome\Application\chrome.exe"),
    ]
    return next((p for p in candidates if os.path.exists(p)), None)


def open_youtube(video_id: str, title: str, artist: str) -> None:
    global _chrome_proc
    url = YOUTUBE_BASE + video_id
    print(f"[player] Opening: {title} — {artist}")
    print(f"[player] URL: {url}")

    # Close the previous song window if still running
    if _chrome_proc is not None:
        if _chrome_proc.poll() is None:  # still alive
            try:
                _chrome_proc.terminate()
                _chrome_proc.wait(timeout=3)
                print("[player] Previous song window closed")
            except Exception as e:
                print(f"[player] Could not close previous window: {e}")
        _chrome_proc = None

    if BROWSER == "default":
        webbrowser.open(url)
        return

    chrome = _find_chrome()
    if chrome:
        # Open as a regular Chrome window so extensions (e.g. uBlock Origin) work
        _chrome_proc = subprocess.Popen([chrome, "--new-window", url],
                                        creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
        print(f"[player] Chrome window opened (pid={_chrome_proc.pid})")
    else:
        print("[player] Chrome not found, falling back to default browser")
        webbrowser.open(url)


# ── mDNS advertisement ───────────────────────────────────────────────────────

def register_mdns(local_ip: str, port: int):
    """Advertise this PC as nora-player.local via mDNS."""
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

    def do_POST(self):
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
        title    = data.get("title",   "Unknown").strip()
        artist   = data.get("artist",  "Unknown").strip()

        if not video_id:
            self._respond(400, {"error": "missing videoId"})
            return

        open_youtube(video_id, title, artist)
        threading.Thread(
            target=_fetch_and_open_thumbnail,
            args=(video_id,),
            daemon=True
        ).start()
        self._respond(200, {"status": "ok", "videoId": video_id})

    def _respond(self, code: int, body: dict) -> None:
        payload = json.dumps(body).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        print(f"[http]  {self.address_string()} — {fmt % args}")


# ── Entry point ───────────────────────────────────────────────────────────────

if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", PORT), Handler)

    # Print the local IP so NORA knows where to POST
    import socket
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        local_ip = s.getsockname()[0]
        s.close()
    except OSError:
        local_ip = "127.0.0.1"

    zeroconf = register_mdns(local_ip, PORT)

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
        if zeroconf:
            zeroconf.unregister_all_services()
            zeroconf.close()
