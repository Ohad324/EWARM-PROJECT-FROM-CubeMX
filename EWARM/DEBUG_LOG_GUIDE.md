# Accessing Macro Output in IAR Debug Log

## Quick Start
1. **Load the macro:**
   ```
   Debug → Macros → Execute Macro → clock_to_mic_bdma.mac
   ```

2. **Open the Debug Log window:**
   - Menu: `View → Debug Log`
   - Or press: `Ctrl+Alt+D`

3. **See the output:**
   - When BP1 fires → All boot-state registers appear in Debug Log
   - Press blue button on board
   - When BP2 fires → All activation-state registers appear in Debug Log

4. **Save the output to file:**
   - Right-click inside the Debug Log window
   - Click `Save`
   - Choose filename: `clock_to_mic_BP1_output.log` (or any name)
   - File saved to your chosen location (default: EWARM folder)

## IAR Debug Log Details

**Window Location:** `View → Debug Log` (Ctrl+Alt+D)

**Features:**
- Captures all `__message` output from macros
- Auto-scrolls as new messages arrive
- Can be docked or floating
- Supports search (Ctrl+F)
- Can be cleared (right-click → Clear)

**Saving Output:**
1. Right-click anywhere in the Debug Log window
2. Select `Save` or `Export`
3. Choose save location and filename
4. File saved as plain text with all captured output

## Output Format

Each breakpoint produces structured output:

**BP1 Output (main.c:226):**
```
===============================================
  BP1: main() entry — Init Phase Diagnostics
===============================================

[RCC] Clock Path:
  RCC_CR           = 0x...
  RCC_D3CCIPR      = 0x...
    SAI4ASEL[7:5]  = 4 (want 4=HSI64)
  ...

[PWR] Power Domains:
  ...

[GPIO PE2] Physical Gate (AF10=SAI4_CK1):
  ...

[SAI4] Configuration (Master):
  ...

[BDMA] Memory Address Check:
  ...
```

**BP2 Output (voice_recorder.c:771):**
```
===============================================
  BP2: Start_Recording_Pipeline() — Active Phase
===============================================

[STAGE 1] PRE-ACTIVATION: GPIO/SAI4 Configuration Check
  ...

[STAGE 2] BDMA Domain Verification (Must be 0x38 for D3)
  ...

[CRITICAL CHECKS AFTER BP2 — During Recording]
  ✓ Step to next line (watch BDMA1_CCR.EN toggle 0→1)
  ...
```

## Troubleshooting Debug Log

**No output appears?**
- Make sure Debug Log window is open (`View → Debug Log`)
- Verify macro is loaded (execute it again)
- Check that breakpoints fired (debug output should appear)
- Look for error messages at bottom of IAR

**Can't find Debug Log window?**
- Workspace might be reset
- Open IAR Embedded Workbench
- Click `View` menu at top
- Select `Debug Log` or search for it in `Window` menu

**Output scrolled off screen?**
- Right-click in Debug Log → `Search` (Ctrl+F)
- Search for `BP1` or `BP2` to find sections
- Or select all (Ctrl+A) and save entire window

## Automated Capture (Optional)

If you want logs automatically saved without manual clicking:

1. Run the Python capture script (if J-Link RTT available):
   ```bash
   python EWARM\capture_debug_log.py
   ```

2. Or use IAR's command-line logging:
   ```bash
   iarbuild MyApplication.ewp -build Debug -log on
   ```

## Next Steps

After saving BP1 and BP2 logs:
1. Review both logs for any errors (e.g., SAI4EN=0, CKABF=1)
2. If all registers match expected values → Recording should work
3. Check `REC_*.wav` file on SD card for audio
4. If no audio, compare BP1/BP2 logs with troubleshooting guide

---

**Files Referenced:**
- Macro: `EWARM/clock_to_mic_bdma.mac`
- Diagnostic Guides: `EWARM/clock_to_mic_BEFORE_REC.md`, `EWARM/clock_to_mic_AFTER_REC.md`
- Full Reference: `EWARM/clock_to_mic_bdma_README.md`
