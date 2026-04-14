"""
format_sd.py — Auto-format STM32 USB MSC SD card as exFAT
===========================================================
Usage:
    python format_sd.py

What it does:
    1. Waits for the STM32 NORA VoiceRec device to appear on USB
       (VID=0483, PID=5720).
    2. Finds the Windows drive letter assigned to that device.
    3. Shows the drive details and asks for confirmation.
    4. Formats as exFAT with label "NORA_REC" using diskpart.
    5. Reports success — board will reset automatically after you
       unplug the CN1 cable.

Requirements:
    - Python 3.8+
    - Run as Administrator (diskpart requires elevated privileges)
    - pywin32:  pip install pywin32
"""

import sys
import time
import ctypes
import subprocess
import tempfile
import os

# --auto flag: skip admin check + skip YES confirmation (used by format_flow.bat)
AUTO_MODE = "--auto" in sys.argv

# ── Target device identity ──────────────────────────────────────────────────
TARGET_VID = "0483"   # STMicroelectronics
TARGET_PID = "5720"   # NORA VoiceRec MSC (usbd_desc.c)
VOLUME_LABEL = "NORA_REC"
POLL_INTERVAL_S = 2
TIMEOUT_S = 120


def is_target_disk(model: str, pnp_device_id: str) -> bool:
    """Return True if the disk looks like the STM32 NORA MSC target."""
    model_u = (model or "").upper()
    pnp_u = (pnp_device_id or "").upper()

    # Primary strong match: explicit product model exposed by the firmware.
    if "NORA VOICEREC USB DEVICE" in model_u:
        return True

    # Fallback for boards exposing VID/PID through PNPDeviceID.
    return (f"VID_{TARGET_VID}" in pnp_u and f"PID_{TARGET_PID}" in pnp_u)


def is_admin():
    try:
        return ctypes.windll.shell32.IsUserAnAdmin()
    except Exception:
        return False


def run_ps(cmd: str) -> str:
    """Run a PowerShell command and return stdout."""
    result = subprocess.run(
        ["powershell", "-NoProfile", "-Command", cmd],
        capture_output=True, text=True
    )
    return result.stdout.strip()


def find_stm32_disk() -> tuple[str | None, int | None]:
    """
    Returns (drive_letter, disk_number) of the STM32 MSC device,
    or (None, None) if not found.
    """
    import json

    ps = r"""
$items = Get-WmiObject Win32_DiskDrive |
    Where-Object { $_.InterfaceType -eq 'USB' } |
    Select-Object Index, Size, Model, PNPDeviceID
if ($items) { $items | ConvertTo-Json -Compress }
"""
    out = run_ps(ps).strip()
    if not out or out == "null":
        return None, None

    try:
        data = json.loads(out)
        entries = data if isinstance(data, list) else [data]
        match = None
        for e in entries:
            if is_target_disk(e.get("Model", ""), e.get("PNPDeviceID", "")):
                match = e
                break

        if match is None:
            return None, None

        disk_index = int(match["Index"])
        size_gb    = int(match.get("Size") or 0) // (1024 ** 3)
        model      = match.get("Model", "Unknown")
        print(f"  Found target: Disk {disk_index}  {size_gb} GB  [{model}]")
    except Exception as e:
        print(f"  Parse error: {e}  raw={out}")
        return None, None

    ps_letter = f"""
$letter = (Get-Partition -DiskNumber {disk_index} -ErrorAction SilentlyContinue |
           Get-Volume -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty DriveLetter)
if ($letter) {{ $letter }} else {{ '' }}
"""
    letter = run_ps(ps_letter).strip().strip('"').upper()
    if len(letter) == 1 and letter.isalpha():
        return letter, disk_index
    return None, disk_index


def list_usb_disks_debug() -> None:
    """Print currently visible USB disks and whether VID/PID match target."""
    import json

    ps = r"""
$items = Get-WmiObject Win32_DiskDrive |
    Where-Object { $_.InterfaceType -eq 'USB' } |
    Select-Object Index, Model, PNPDeviceID
if ($items) { $items | ConvertTo-Json -Compress }
"""
    out = run_ps(ps).strip()
    if not out or out == "null":
        print("    USB disks: none")
        return

    try:
        data = json.loads(out)
        entries = data if isinstance(data, list) else [data]
        print("    USB disks visible:")
        for e in entries:
            idx = e.get("Index", "?")
            model = e.get("Model", "Unknown")
            pnp = (e.get("PNPDeviceID") or "")
            is_target = is_target_disk(model, pnp)
            mark = "TARGET" if is_target else "other"
            print(f"      - Disk {idx}: {model} [{mark}]")
    except Exception as e:
        print(f"    USB disk parse error: {e}")


def eject_disk(disk_number: int):
    """
    Offline the disk via diskpart → Windows suspends USB →
    STM32 detects dev_state change → fires STAGE('FMT DONE') → resets.
    """
    script = f"select disk {disk_number}\noffline disk\nexit\n"
    with tempfile.NamedTemporaryFile(mode='w', suffix='.txt', delete=False) as f:
        f.write(script)
        path = f.name
    print("  Ejecting disk — STM32 will detect USB suspend and reset automatically...")
    subprocess.run(["diskpart", "/s", path], capture_output=True)
    os.unlink(path)


def format_with_diskpart(disk_number: int) -> bool:
    """
    Use diskpart to:
      - clean the disk
      - create a primary partition
      - format as exFAT with label NORA_REC
      - assign a drive letter
    """
    script = f"""select disk {disk_number}
clean
create partition primary
format fs=exfat label={VOLUME_LABEL} quick
assign
exit
"""
    with tempfile.NamedTemporaryFile(mode='w', suffix='.txt',
                                    delete=False) as f:
        f.write(script)
        script_path = f.name

    print(f"\n  Running diskpart (script: {script_path}) ...")
    result = subprocess.run(
        ["diskpart", "/s", script_path],
        capture_output=True, text=True
    )
    os.unlink(script_path)

    if result.returncode == 0:
        print(result.stdout)
        eject_disk(disk_number)
        return True
    else:
        print(f"  diskpart FAILED (rc={result.returncode})")
        print(result.stdout)
        print(result.stderr)
        return False


def wait_for_device() -> tuple[str | None, int | None]:
    """Poll until the STM32 device appears, up to TIMEOUT_S seconds."""
    print(f"Waiting for USB disk on CN1 (STM32 MSC)...")
    print(f"  → Board will enter format mode automatically after flash")
    print()

    elapsed = 0
    while elapsed < TIMEOUT_S:
        print(f"  [{elapsed:3d}s] probing USB disks...")
        list_usb_disks_debug()
        drive, disk = find_stm32_disk()
        if disk is not None:
            return drive, disk
        time.sleep(POLL_INTERVAL_S)
        elapsed += POLL_INTERVAL_S

    return None, None


def main():
    print("=" * 60)
    print("  NORA SD Card Formatter — exFAT")
    print("=" * 60)
    print()

    if not AUTO_MODE and not is_admin():
        print("ERROR: This script must be run as Administrator.")
        print("  Right-click → 'Run as administrator'  or")
        print("  Run from an elevated command prompt.")
        sys.exit(1)

    if AUTO_MODE:
        print("  [AUTO] Running unattended — no confirmation required.")
        print()

    drive_letter, disk_number = wait_for_device()

    if disk_number is None:
        print(f"\nTimeout ({TIMEOUT_S}s) — device not found. Check:")
        print("  1. Blue button held 4s at boot?")
        print("  2. CN1 cable plugged in after button held?")
        print("  3. Board powered (CN2 connected)?")
        sys.exit(1)

    print(f"\nDevice found: Disk {disk_number}", end="")
    if drive_letter:
        print(f"  (drive {drive_letter}:)")
    else:
        print("  (no partition/letter — blank card)")

    print()
    print(f"  This will ERASE ALL DATA on Disk {disk_number}")
    print(f"  and format it as exFAT with label '{VOLUME_LABEL}'.")
    print()

    if not AUTO_MODE:
        confirm = input("  Type YES to continue: ").strip()
        if confirm != "YES":
            print("Cancelled.")
            sys.exit(0)
    else:
        print("  [AUTO] Proceeding automatically...")

    print()
    ok = format_with_diskpart(disk_number)

    if ok:
        print()
        print("=" * 60)
        print("  FORMAT COMPLETE")
        print(f"  SD card is now exFAT / label={VOLUME_LABEL}")
        print()
        print("  → CN1 ejected automatically — board resets")
        print("  → On next boot f_mount will succeed → recording ready")
        print("=" * 60)
    else:
        print()
        print("FORMAT FAILED — see diskpart output above.")
        sys.exit(1)


if __name__ == "__main__":
    main()
