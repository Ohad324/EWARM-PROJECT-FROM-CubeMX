# C-SPY Bat and C-SPY Macros in Practice

## Introduction

IAR gives us two closely related debugging tools that solve different parts of the same problem:

- **C-SPY Bat (`cspybat.exe`)** is the **headless command-line debugger runner**.
- **C-SPY macro files (`.mac`)** are the **automation language** used inside the debugger.

The easiest way to think about them is this:

- `cspybat` is the **engine** that launches and controls a debug session from a script or CI-like flow.
- a `.mac` file is the **playbook** that tells the debugger what to do once connected.

In practice, these two tools are most useful when failures are timing-sensitive, happen early in boot, or are easier to validate from raw register state than from application logs.

Typical usage patterns include:

- headless boot-sequence verification
- timed peripheral register snapshots
- interactive breakpoint diagnostics
- attach-and-trigger flows against a running board

This article explains what each tool is good at, when we used it, the main API surface we relied on, and the syntax patterns that matter in real work.

---

## 1. What C-SPY Bat Is Good For

### What it is

`cspybat.exe` is IAR’s non-GUI way to run the same debugger backend that the IDE uses. It can:

- load an executable
- connect to a probe/backend
- flash firmware
- attach to a running target
- run one or more macro files
- write output to log files
- exit cleanly for use from batch files, PowerShell, or VS Code tasks

In other words, `cspybat` is ideal when you want **repeatable debugging without opening the IDE**.

### Main benefits of C-SPY Bat

#### 1. Repeatability

Manual GUI debugging is useful for discovery, but it is weak for comparison. A batch flow can rebuild, flash, attach, wait fixed amounts of time, run a macro, and capture the result the same way every time.

That is exactly why scripted runtime checks and boot probes are valuable. They turn debugging into a controlled experiment.

#### 2. Early-boot visibility

Many bugs happen before RTT, semihosting, or the UI is reliable. In those cases, a debugger-driven register read is much more trustworthy than application logging.

For the LCD boot-order work, `cspybat` let us answer questions like:

- Is LTDC still disabled at the expected early stage?
- Is DSI enabled only after the intended handoff?
- Did the framebuffer get painted by a given time?
- Are sticky LTDC error flags such as FUIF already set?

Those are all easier to measure from peripheral registers than from logs.

#### 3. Automation from tasks and scripts

`cspybat` fits naturally into `.bat` and PowerShell wrappers. That makes it useful for:

- one-click debug checks
- regression probes
- support scripts for other developers
- nightly or ad hoc validation runs

In this workspace, the batch layer usually does the orchestration:

1. kill any stale debugger processes
2. rebuild with `iarbuild`
3. flash the image
4. run `cspybat`
5. save the debugger output to a log file

### When we used C-SPY Bat in this project

We used it in three main ways:

#### A. Boot-sequence verification

Purpose:

- verify a staged boot sequence without relying on RTT
- sample LTDC, DSI, MPU, and framebuffer state at defined time offsets

Why `cspybat` was a good fit:

- the checks had to happen at deterministic times after reset
- we wanted a text log with PASS/FAIL style output
- the sequence needed to be rerun many times after code changes

#### B. Timed runtime snapshots

Purpose:

- take snapshots at boot, idle, mid-recording, and post-recording
- compare raw DFSDM / DMA register state over time

Why `cspybat` was a good fit:

- it could run unattended
- it produced a stable text log
- it reduced human timing error when taking snapshots manually

#### C. Attach to a running system and perform one action

Purpose:

- connect to a running board
- execute a macro that fires EXTI13 or reads a small set of registers
- leave the target running if needed

This is a good middle ground between “full GUI session” and “fully scripted rebuild + flash + probe”.

The key `cspybat` option for this pattern is:

```text
--attach_to_running_target
```

This tells the debugger to connect to firmware that is already running instead of performing a fresh download-and-reset cycle first. It is especially useful when you want to observe a live system, trigger one action, inspect register state, and then detach.

Another commonly paired option is:

```text
--leave_target_running
```

That combination is useful when you want the target to keep running after the scripted check completes.

---

## 2. What C-SPY Macro Files Are Good For

### What a `.mac` file is

A C-SPY macro file is not full C. It is a small debugger scripting language with C-like expressions and debugger-specific built-ins.

Important limitation:

- it does **not** support the C preprocessor
- no `#define`
- no normal header inclusion model

That is why inline literals or `__var` locals are the safe pattern in `.mac` files.

### Main benefits of C-SPY macros

#### 1. Direct debugger automation

Macros let you tell the debugger to:

- read and write memory-mapped registers
- place breakpoints with actions
- print structured diagnostic output
- run and halt the core
- wait fixed amounts of time
- evaluate symbols in the target image

This is much more powerful than a plain batch file. The batch file can launch the debugger, but the macro is what performs the debug logic.

#### 2. Register-centric debugging

A large fraction of embedded bring-up problems are fundamentally about hardware state:

- wrong clock source
- wrong enable bit
- wrong GPIO alternate function
- wrong DMA address
- sticky interrupt flags

Macros are excellent for this because `__readMemory32()` and `__writeMemory32()` let you inspect exactly what the hardware sees.

Typical examples include breakpoint-driven peripheral tracing, one-shot register probes, and timing-based boot-state checks.

#### 3. Breakpoint actions without touching firmware

One of the biggest advantages of macros is that they let you instrument behavior **without editing the firmware**.

That matters when:

- the code is fragile
- logging changes timing
- release builds strip debug output
- you want to avoid yet another temporary patch

In a typical macro-based diagnostic flow, breakpoints are created with `__setCodeBreak(...)`, and each breakpoint emits structured information when it hits. That gives us a lightweight trace of the pipeline without changing application code.

### When we used C-SPY macros in this project

#### A. Breakpoint-driven pipeline tracing

Used for the DFSDM and DMA pipeline.

Goal:

- prove whether callbacks fired
- inspect FLTCR1 before and after start
- inspect NDTR and buffer contents
- decide whether the bug was clocking, HAL configuration, or DMA movement

#### B. One-shot register probes

Used when a single snapshot was enough.

Goal:

- read GPIO mode/AF registers
- read SAI4 / DFSDM state
- print a short diagnosis to the debug log

#### C. Timed boot-state checks

Goal:

- `__go()`
- `__delay()`
- `__stop()`
- inspect state at each milestone

This is exactly the right use case for macros: a deterministic timeline plus direct hardware reads.

---

## 3. C-SPY Bat vs. C-SPY Macro: Which One Solves What?

The clean split is:

- use **`cspybat`** when you need a reproducible debug session from the command line
- use **`.mac`** when you need the debugger to perform actual inspection, logging, breakpoints, or control logic

### Use C-SPY Bat when

- you want to automate a flow from a batch file or PowerShell
- you need rebuild + flash + attach + log capture
- you want a repeatable regression probe
- you want the same debug sequence to run in VS Code tasks or scripts

### Use a C-SPY macro when

- you need to read/write registers
- you need time-based staged checks
- you need breakpoint actions
- you need structured `__message` output in the debug log
- you want to inspect target symbols without recompiling firmware instrumentation

### Use both together when

- the debug session must be automated end-to-end
- the logic is too rich for a pure shell script
- you want logs that are useful after the session ends

In real embedded workflows, the strongest automated debug flows usually use both. The batch layer handles process control and environment setup; the macro handles target-side inspection.

---

## 4. The API Surface We Actually Used

Below are the most useful C-SPY macro features for this project.

### Common `cspybat` command-line options

Although the macro language does the actual inspection work, a few `cspybat` options matter enough that they should be treated as part of the practical API surface:

### `--download_only`

Downloads the image to the target without starting a full interactive IDE session.

### `--attach_to_running_target`

Attaches to firmware that is already executing on the board.

This is the right option when you want to inspect a live state machine, trigger a one-shot action, or sample registers without disturbing boot timing with a reset.

### `--leave_target_running`

Leaves the firmware running after the scripted debugger session ends.

### `--macro=<file>`

Runs a macro file automatically after the debugger session starts.

### `--backend`

Passes the remaining options through to the debugger backend and probe driver.

### Generic attach example

```text
cspybat.exe --attach_to_running_target --leave_target_running --macro=probe.mac ...
```

### `__var`

Declares a macro variable.

```c
__var gcr, dsi_cr, dsi_wcr;
```

### `__message`

Prints text to the IAR Debug Log or the `cspybat` output stream.

```c
__message "LTDC GCR = 0x", gcr:%08X, "\n";
```

This is the main reporting tool in nearly every macro we used.

### `__readMemory32(address, "Memory")`

Reads a 32-bit value from memory or a peripheral register.

```c
gcr = __readMemory32(0x50001018, "Memory");
```

This was the workhorse API for LTDC, DSI, DFSDM, DMA, GPIO, and framebuffer checks.

### `__writeMemory32(value, address, "Memory")`

Writes a 32-bit value to memory or a peripheral register.

```c
__writeMemory32(0x0F, 0x5000103C, "Memory");
```

We used this for actions such as clearing sticky flags or firing a software trigger.

### `__go()`, `__stop()`, `__delay(ms)`

Used for timed state sampling.

```c
__go();
__delay(500);
__stop();
```

This pattern is the core of any staged boot probe.

### `__setCodeBreak(location, ...)`

Creates a code breakpoint with an action string.

Example pattern from [EWARM/dfsdm_debug.mac](c:/TouchGFXProjects/MyApplication/EWARM/dfsdm_debug.mac):

```c
bp1 = __setCodeBreak("{voice_recorder.c}.242", 0, "1", "TRUE",
    "__message \"[BP1] DFSDM_HW_Init() entered\\n\"");
```

This is extremely useful when you want debugger-driven logging at specific code lines.

### `__symbolAddress("name")`

Gets the runtime address of a symbol from the loaded image.

```c
__readMemory32(__symbolAddress("g_SampleCount"), "Memory");
```

This is safer than hard-coding addresses for global variables when the linker layout may move.

---

## 5. Syntax Examples That Matter

### Example 1: simple helper function

```c
PrintHex32(label, v)
{
    __message "  ", label, " = 0x", v:%08X, "\n";
}
```

### Example 2: register check

```c
CheckBit(label, val, bit, expect_set)
{
    __var got;
    got = (val >> bit) & 1;
    if (got == expect_set)
        __message "  PASS ", label, "\n";
    else
        __message "  FAIL ", label, "\n";
}
```

### Example 3: read a peripheral register

```c
Stage_PhaseOne_Mid()
{
    __var gcr;
    gcr = __readMemory32(0x50001018, "Memory");
    __message "LTDC->GCR = 0x", gcr:%08X, "\n";
}
```

### Example 4: timed boot probe

```c
execUserSetup()
{
    __go();
    __delay(50);
    __stop();
    Stage_PhaseOne_Mid();
}
```

### Example 5: breakpoint with an action

```c
execUserSetup()
{
    __var bp;
    bp = __setCodeBreak("{voice_recorder.c}.388", 0, "1", "TRUE",
        "__message \"[BP] Drain loop entered\\n\"");
}
```

### Example 6: complete C-SPY macro file

The example below is a small but realistic macro file. It attaches debugger-side logic to a running session, reads a peripheral register, prints a short diagnosis, lets the target run for a fixed delay, then stops again for a second snapshot.

```c
PrintReg(label, addr)
{
    __var value;
    value = __readMemory32(addr, "Memory");
    __message label, " = 0x", value:%08X, "\n";
}

CheckBitSet(label, addr, bit)
{
    __var value;
    __var state;

    value = __readMemory32(addr, "Memory");
    state = (value >> bit) & 1;

    if (state == 1)
        __message "PASS  ", label, " bit ", bit:%d, " is set\n";
    else
        __message "FAIL  ", label, " bit ", bit:%d, " is clear\n";
}

Snapshot()
{
    __message "--- Snapshot ---\n";
    PrintReg("Peripheral control", 0x50001018);
    CheckBitSet("Peripheral enabled", 0x50001018, 0);
}

execUserSetup()
{
    __message "Starting macro session\n";

    __stop();
    Snapshot();

    __message "Running target for 250 ms\n";
    __go();
    __delay(250);
    __stop();

    Snapshot();
    __message "Macro session complete\n";
}
```

This example demonstrates the most common macro-file structure:

- small helper functions for repeated formatting
- one or more snapshot or check functions
- an `execUserSetup()` entry point that controls the debug session

### Important language limitation

Do not write this in a `.mac` file:

```c
#define LTDC_GCR 0x50001018
```

That is normal C preprocessor syntax, and the C-SPY macro parser does not support it. Use inline literals or assign the address to a `__var` instead.

Valid pattern:

```c
__var ltdc_gcr;
ltdc_gcr = 0x50001018;
```

---

## 6. Best Practices from This Project

### Keep the shell and the macro separate

Put process management in `.bat` or `.ps1`, and keep target inspection in `.mac`.

Good pattern:

- batch file handles build, flash, attach, log path
- macro handles breakpoints, register reads, timing checks

### Prefer register reads over firmware edits for first-pass diagnosis

For peripheral bring-up and boot sequencing, a debugger-side read is often the fastest path to truth.

### Use symbols for globals, literals for peripherals

- globals: prefer `__symbolAddress()` when possible
- hardware registers: inline literal addresses are often simplest and most stable

### Make macro output opinionated

Do not only dump values. Also decode them.

The best macros in this project do both:

- print the register value
- print a PASS/FAIL expectation
- explain what a bad result means

That makes the output useful even when read days later from a log file.

### Accept that C-SPY macros are a small language

They are very useful, but they are not general-purpose scripting. Once the logic becomes large or string-heavy, complexity rises quickly. That is the point where a J-Link script, host-side Python, or a different capture method may be a better fit.

---

## Conclusion

`cspybat` and C-SPY macros are best used together, not as alternatives.

- `cspybat` gives you **repeatable headless sessions**.
- `.mac` files give you **debugger-side logic and hardware inspection**.

In this project, they were especially effective for:

- boot-sequence debugging
- DFSDM and DMA path verification
- attach-and-trigger automation
- breakpoint-driven diagnosis without touching the firmware

For STM32 bring-up and fragile timing bugs, that combination is often stronger than adding temporary logs. It is deterministic, low-intrusion, and produces artifacts that can be compared across runs.

If you need a rule of thumb:

- use `cspybat` to automate the **session**
- use `.mac` to automate the **debug thinking inside the session**
