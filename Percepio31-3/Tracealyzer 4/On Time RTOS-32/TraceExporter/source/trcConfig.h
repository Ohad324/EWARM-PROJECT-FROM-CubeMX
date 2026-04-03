/******************************************************************************* 
 * On Time RTOS-32 Trace Exporter Library v1.5
 * Percepio AB, www.percepio.com
 *
 * trcConfig.h
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

#ifndef TRC_CONFIG_H
#define TRC_CONFIG_H

/****************************************************************
*	TRACE_USE_VERBOSE_XML
*	This value indicates whether the verbose XML format should
*	be used. It takes up more space but is easier to read.
****************************************************************/
#define TRACE_USE_VERBOSE_XML				1

/****************************************************************
*	TRACE_XML_ENCODING
*	This string indicates what type of encoding the XML file
*	should use. Default is "ISO-8859-1".
****************************************************************/
#define TRACE_XML_ENCODING					"ISO-8859-1"

/****************************************************************
*	TRACE_MAX_OBJECTS
*	This value indicates the maximum number of objects that the
*	exporter should be able to handle.
*	Modify it to suit the target system.
****************************************************************/
#define TRACE_MAX_OBJECTS					128

/****************************************************************
*	TRACE_USING_MULTIPROCESSOR_KERNEL
*	Use this define if the multi processor kernel (Rtk32mp.lib)
*	is used. This will allow the exporter to make use of
*	RTKCPUs().
****************************************************************/
#define TRACE_USING_MULTIPROCESSOR_KERNEL

/****************************************************************
*	TRACE_MAX_CPUS
*	This value indicates the maximum number of CPUs that the
*	exporter should be able to handle.
*	Modify it to suit the target system.
****************************************************************/
#define TRACE_MAX_CPUS						16

/****************************************************************
*	TRACE_MAX_NAME_LENGTH
*	This value indicates the maximum acceptable size of names for
*	objects. Only needed for the names of spinlocks since no
*	flag is available to indicate wether the spinlock object is
*	valid or not (unlike tasks, semaphores and mailboxes).
*	Modify it to suit the target system.
****************************************************************/
#define TRACE_MAX_NAME_LENGTH				32

/****************************************************************
*	TRACE_INTERRUPT_CONTEXT_SWITCH_TIME
*	Since tInterruptStart and tInterruptEnd is no longer used
*	in On Time RTOS-32, this value indicates the maximum timestamp
*	difference between two subsequent events from an interrupt
*	that will show as the same instance when viewing the trace.
*	A too low value will split a single interrupt instance into
*	multiple instances, while a too high value will sow
*	together two separate instances.
*	Modify it to suit the target system.
*	Value is defined in nanoseconds.
****************************************************************/
#define TRACE_INTERRUPT_CONTEXT_SWITCH_TIME	50000

/****************************************************************
*	TRACE_SEQUENTIAL_TIMESTAMP_MODE
*	This can be defined if the timestamps cannot be trusted due 
*	to poor clocks. The exact timing will be skipped and only the
*	order of the events will be saved. This should be considered
*	a last resort.
****************************************************************/
//#define TRACE_SEQUENTIAL_TIMESTAMP_MODE

#endif
