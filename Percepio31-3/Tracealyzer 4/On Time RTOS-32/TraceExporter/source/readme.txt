Tracealyzer Trace Exporter for On Time RTOS-32
----------------------------------------------
Copyright Percepio AB 2016
www.percepio.com

The purpose of this tool is to export the trace data from the On Time RTOS-32 Kernel Tracer to the Tracealyzer data format.
This gives and XML file that can be read by Tracealyzer for On Time RTOS-32.

The Trace Exporter is delivered in C source code and needs to be included in your embedded application.

Requirements:
The required On Time RTOS-32 components are RTTarget-32 and RTKernel-32.
This tool assumes a Debug kernel of RTOS-32, where the Kernel Tracer is active.
Make sure that RF_TRACE_TIME_NS or RF_TRACE_TIME_US is set in RTKConfig.Flags.

If you notice strange timestamps for the clock interrupt (will be visualized as extremely long interrupts),
a more accurate High Resolution Timer driver is required (see On Time RTOS-32 documentation).

Multi core trace requires kernel version (RTK32_VER) 597 or above.

Usage:
1) Add "trcExport.c" in your build project.
2) Include the header file "trcExport.h" in your source code.
3) Change the defines in "trcConfig.h" to suit your system
4) When you want to generate the trace file:
4 a) Stop the Kernel Tracer by calling RTKStopTracing(), an API function of the Kernel Tracer.
4 b) Call vTracealyzerExport("filename.xml").

Files included:

- readme.txt: this file
- trcConfig.h: the configuration file for the Trace Exporter.
- trcExport.c/.h: the source code of the Trace Exporter.

For technical support, contact support@percepio.com
