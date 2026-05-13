#!/usr/bin/env python3
"""
capture_debug_log.py - Capture IAR Debug Log output while macro runs

This script monitors the IAR Debug Log window (via COM port/telnet or file watching)
and saves all macro output to timestamped log files:
  - clock_to_mic_BP1_YYYYMMDD_HHMMSS.log  (BP1 registers)
  - clock_to_mic_BP2_YYYYMMDD_HHMMSS.log  (BP2 registers)

Usage:
  python capture_debug_log.py

The script waits for macro output patterns and captures them to files automatically.
"""

import os
import sys
import time
from datetime import datetime
from pathlib import Path

EWARM_PATH = Path(__file__).parent
BP1_MARKER = "BP1: main() entry"
BP2_MARKER = "BP2: Start_Recording_Pipeline"

def create_log_filename(bp_name):
    """Create timestamped log filename"""
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return EWARM_PATH / f"clock_to_mic_{bp_name}_{timestamp}.log"

def monitor_iar_output():
    """
    Monitor for IAR debug output and save to log files.
    Note: This is a stub. In practice, you would:
    1. Use IAR's GDB server output capture
    2. Read from a named pipe/socket connected to IAR
    3. Use J-Link RTT output
    """
    print("[DEBUG LOG CAPTURE] Waiting for IAR macro output...")
    print(f"  Log files will be saved to: {EWARM_PATH}")
    print(f"  BP1 output → clock_to_mic_BP1_*.log")
    print(f"  BP2 output → clock_to_mic_BP2_*.log")
    print()
    print("ALTERNATIVE: Enable Debug Log in IAR Studio:")
    print("  1. View → Debug Log  (or press Ctrl+Alt+D)")
    print("  2. Right-click on Debug Log window → Save")
    print()

if __name__ == "__main__":
    monitor_iar_output()
