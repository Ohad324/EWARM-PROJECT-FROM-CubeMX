/******************************************************************************* 
 * On Time RTOS-32 Trace Exporter Library v1.5
 * Percepio AB, www.percepio.com
 *
 * trcExport.h
 *
 * Terms of Use
 * This software is copyright Percepio AB. The trace exporter library is free for
 * use together with Percepio products. You may distribute the trace exporter
 * library in its original form, including modifications given that these 
 * modifications are clearly marked as your own modifications and documented
 * in the initial comment section of these source files.
 * This software is the intellectual property of Percepio AB and may not be 
 * sold or in other ways commercially redistributed without explicit written 
 * permission by Percepio AB.
 *
 * Disclaimer 
 * The trace tool and trace exporter library is being delivered to you AS IS and 
 * Percepio AB makes no warranty as to its use or performance. Percepio AB does 
 * not and cannot warrant the performance or results you may obtain by using the 
 * software or documentation. Percepio AB make no warranties, express or 
 * implied, as to noninfringement of third party rights, merchantability, or 
 * fitness for any particular purpose. In no event will Percepio AB, its 
 * technology partners, or distributors be liable to you for any consequential, 
 * incidental or special damages, including any lost profits or lost savings, 
 * even if a representative of Percepio AB has been advised of the possibility 
 * of such damages, or for any claim by any third party. Some jurisdictions do 
 * not allow the exclusion or limitation of incidental, consequential or special 
 * damages, or the exclusion of implied warranties or limitations on how long an 
 * implied warranty may last, so the above limitations may not apply to you.
 *
 * Copyright Percepio AB, 2016.
 * www.percepio.com
 ******************************************************************************/

#ifndef TRC_EXPORT_H
#define TRC_EXPORT_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
*	vTracealyzerExport
*	Entry function that will export everything in the RTKTraceBuffer to an
*	XML format used by Tracealyzer.
*	NOTE: The trace data collection should be stopped before this using
*	RTKStopTracing!
****************************************************************************/
void vTracealyzerExport(const char* szFileName);

/****************************************************************************
*	vTracealyzerAssignUserEventAsExtendedData
*	Call this function to assign an user event as extended data.
*	This will allow manual interrupt starts and ends to be flagged using
*	vTracealyzerInterruptStart and vTracealyzerInterruptEnd since
*	tInterruptStart and tInterruptEnd is no longer used by On Time RTOS-32.
****************************************************************************/
void vTracealyzerAssignUserEventAsExtendedData(RTKTraceEvent tUserEvent);

/****************************************************************************
*	vTracealyzerInterruptStart
*	Call this function to indicate the start of an interrupt.
*	NOTE: A user event must first be assigned as extended data using
*	vTracealyzerSetUserEventAsExtendedData().
****************************************************************************/
void vTracealyzerExtendedInterruptStart();

/****************************************************************************
*	vTracealyzerInterruptEnd
*	Call this function to indicate the end of an interrupt.
*	NOTE: A user event must first be assigned as extended data using
*	vTracealyzerSetUserEventAsExtendedData().
****************************************************************************/
void vTracealyzerExtendedInterruptEnd();

/****************************************************************************
*	vDumpTraceBuffer
*	Simple helper function that dumps the entire trace buffer to a file.
*	NOTE: The trace data collection should be stopped before this using
*	RTKStopTracing!
****************************************************************************/
void vDumpTraceBuffer(const char* szFileName);

#ifdef  __cplusplus
}
#endif

#endif
