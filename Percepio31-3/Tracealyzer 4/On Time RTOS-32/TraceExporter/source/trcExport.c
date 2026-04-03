/******************************************************************************* 
 * On Time RTOS-32 Trace Exporter Library v1.5
 * Percepio AB, www.percepio.com
 *
 * trcExport.c
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

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <Rttarget.h>

#define RTK_INT_64 unsigned long long

#include <Rtk32.h>
#include <Finetime.h>
#include "trcConfig.h"
#include "trcExport.h"

#define MAX_LINE_LENGTH			256
#define MAX_STACK_SIZE			16

#define LegalTCBId     0x47110815u
#define LegalSemaId    0x6A7B8C9Du
#define LegalMCBId     0x5B4C3D2Eu

#define RTKUnknown			0
#define RTKWait				1
#define RTKSignal			2
#define RTKPut				3
#define RTKGet				4
#define RTKSend				5
#define RTKReceive			6
#define RTKLockSpinlock		7
#define RTKReleaseSpinlock	8
#define RTKSetPriority		9
#define RTKDelayUntil		10
#define RTKDeleteSemaphore	11
#define RTKDeleteSpinlock	12
#define RTKDeleteMailbox	13
#define RTKPulse			14
#define RTKResetEvent		15

#define tNewInstance				tDummy - 1
#define tExtendedInterruptStart		tDummy - 2
#define tExtendedInterruptEnd		tDummy - 3

#define EXTENDED_INTERRUPT_START	0x01000000
#define EXTENDED_INTERRUPT_END		0x02000000
#define EXTENDED_TYPE_MASK			0xFF000000
#define EXTENDED_DATA_MASK			~EXTENDED_TYPE_MASK

#define FILE_OPEN(filename) hXmlFile = szFileName ? fopen(szFileName, "wb") : stdout;
#define FILE_WRITE(str) fwrite((str), strlen(str), 1, hXmlFile);
#define FILE_CLOSE() if (hXmlFile != stdout) fclose(hXmlFile);

#define NUM_EVENTS min((int)RTKTraceBuffer->BufferEnd - 1, (int)RTKTraceBuffer->BufferSizeMask)

#define RESULT_Unknown				0
#define RESULT_InstantSuccess		1
#define RESULT_ReturnSuccess		2
#define RESULT_Block				3
#define RESULT_Timeout				4
#define RESULT_InstantFail			5

#define IS_SEMAPHORE_EVENT_BIT			32
#define IS_SEMAPHORE_EVENT_MASK			(1 << (IS_SEMAPHORE_EVENT_BIT - 1))
#define VALID_TO_MASK				(~IS_SEMAPHORE_EVENT_MASK)

#if !defined TRACE_USE_VERBOSE_XML || TRACE_USE_VERBOSE_XML == 1

#define TRACE_XML_TraceInformation "TraceInformation"
#define TRACE_XML_Events "Events"

#define TRACE_XML_IsrPriorityDirection "IsrPriorityDirection"
#define TRACE_XML_TargetPlatform "TargetPlatform"
#define TRACE_XML_TickFrequency "TickFrequency"
#define TRACE_XML_NumCpus "NumCpus"

#define TRACE_XML_ObjectTable "ObjectTable"
#define TRACE_XML_Object "Object"

#define TRACE_XML_ServiceTable "ServiceTable"
#define TRACE_XML_Service "Service"

#define TRACE_XML_ContextSwitchEvent "ContextSwitchEvent"
#define TRACE_XML_ActorReadyEvent "ActorReadyEvent"
#define TRACE_XML_KernelServiceEvent "KernelServiceEvent"
#define TRACE_XML_KernelNoticeEvent "KernelNoticeEvent"
#define TRACE_XML_PriorityChangeEvent "PriorityChangeEvent"
#define TRACE_XML_UserEvent "UserEvent"

#define TRACE_XML_timestamp "timestamp"
#define TRACE_XML_actor "actor"
#define TRACE_XML_service "service"
#define TRACE_XML_referencedObject "referencedObject"
#define TRACE_XML_numericParameter "numericParameter"
#define TRACE_XML_result "result"
#define TRACE_XML_string "string"
#define TRACE_XML_value "value"
#define TRACE_XML_code "code"
#define TRACE_XML_name "name"
#define TRACE_XML_class "class"
#define TRACE_XML_priority "priority"
#define TRACE_XML_newInstance "newInstance"
#define TRACE_XML_channel "channel"
#define TRACE_XML_direction "direction"
#define TRACE_XML_CPU "cpu"

static char results[6][20] = {"Unknown", "InstantSuccess", "ReturnSuccess", "Block", "Timeout", "InstantFail"};

#else

#define TRACE_XML_TraceInformation "info"
#define TRACE_XML_Events "evts"

#define TRACE_XML_IsrPriorityDirection "ipriodir"
#define TRACE_XML_TargetPlatform "tp"
#define TRACE_XML_TickFrequency "freq"
#define TRACE_XML_NumCpus "NumCpus"

#define TRACE_XML_ObjectTable "objtbl"
#define TRACE_XML_Object "obj"

#define TRACE_XML_ServiceTable "svctbl"
#define TRACE_XML_Service "svc"

#define TRACE_XML_ContextSwitchEvent "cse"
#define TRACE_XML_ActorReadyEvent "are"
#define TRACE_XML_KernelServiceEvent "kse"
#define TRACE_XML_KernelNoticeEvent "kne"
#define TRACE_XML_PriorityChangeEvent "pce"
#define TRACE_XML_UserEvent "ue"

#define TRACE_XML_timestamp "ts"
#define TRACE_XML_actor "a"
#define TRACE_XML_service "svc"
#define TRACE_XML_referencedObject "obj"
#define TRACE_XML_numericParameter "num"
#define TRACE_XML_result "res"
#define TRACE_XML_string "str"
#define TRACE_XML_value "val"
#define TRACE_XML_code "c"
#define TRACE_XML_name "n"
#define TRACE_XML_class "cl"
#define TRACE_XML_priority "prio"
#define TRACE_XML_newInstance "ni"
#define TRACE_XML_channel "ch"
#define TRACE_XML_direction "dir"
#define TRACE_XML_CPU "cpu"

static char results[6][20] = { "u", "is", "rs", "b", "t", "if" };

#endif

#define FILE_WRITE_HELPER(code, name) FILE_WRITE("\t\t\t<"TRACE_XML_Service" "TRACE_XML_code"=\"" #code "\" "TRACE_XML_name"=\"" name "\" />\r\n");
#define FILE_WRITE_SERVICE(service) FILE_WRITE_HELPER(service, #service)
#define FILE_WRITE_TIMESTAMP_WARNING() if (hasWarnedTimestamps == 0) { FILE_WRITE("<TraceWarning "TRACE_XML_string"=\"Detected out of order timestamps!\r\nUse a better clock (HRTHPET) or enable sequential mode in the exporter configuration (TRACE_SEQUENTIAL_TIMESTAMP_MODE).\r\nIf unsynced clocks are used in a multi CPU setup, try synchronizing them periodically using RTMPSynchTimeStampCounters().\" />\r\n"); hasWarnedTimestamps = 1; }

static FILE* hXmlFile;

struct rtkSpinlock
{
	DWORD Reserved[9];
	const char* Name;
};

struct objectWrapper
{
	unsigned int object;
	unsigned int state;
	unsigned int refObject;
	unsigned int validToAndIsEvent;
};

struct cpuContextWrapper
{
	unsigned int interruptStack[MAX_STACK_SIZE];
	unsigned int interruptStackIndex;
	unsigned int currentSourceObjectIndex;
	struct rtkTraceRec* pPreviousEvent;
	int previousLevel;
};

int hasWarnedTimestamps = 0;
unsigned long long timestampPadding = 0;
DWORD previousTimestamp = 0;
static unsigned int objectCounter = 0;
static unsigned int CPUs = 1;
static struct objectWrapper objects[TRACE_MAX_OBJECTS] = {0};
static struct cpuContextWrapper cpuContext[TRACE_MAX_CPUS];
static unsigned int interruptContextSwitchTime = TRACE_INTERRUPT_CONTEXT_SWITCH_TIME;

static RTKTraceEvent extendedDataEvent = tNoEvent;

static void prvTraceExport();
static void prvExportObjectTable();
static void prvExportServicesAndObjects();
static void prvExportEvents();
static void prvGetEventResult(RTKTraceEvent, int*, int*, const int);
static int prvGetTaskIndex(unsigned int, unsigned int, struct rtkTraceRec* rec);
static int prvGetCPUFromEvent(struct rtkTraceRec* rec);
static unsigned long long prvGetTimestampFromEvent(struct rtkTraceRec* rec);
static unsigned long long prvGetTimestampDiff(struct rtkTraceRec* high, struct rtkTraceRec* low);
static int prvGetObjectIndex(unsigned int, unsigned int);
static void prvAddObject(unsigned int);
static struct rtkTraceRec* prvGetTraceRecord(int);
unsigned int prvIsSemaphoreEvent(int, int);
unsigned int prvIsValidName(const char*);
unsigned int prvIsValidNameChar(char);

/****************************************************************************
*	vDumpTraceBuffer
*	Simple helper function that dumps the entire trace buffer to a file.
*	NOTE: The trace data collection should be stopped before this using
*	RTKStopTracing!
****************************************************************************/
void vDumpTraceBuffer(const char* szFileName)
{
	char buffer[2048];
	int i;

	FILE_OPEN(szFileName);

	RTKTraceHeader(buffer);
	FILE_WRITE(buffer);
	for (i = NUM_EVENTS; i >= 0; i--)
	{
		RTKDumpTrace(buffer, i);
		FILE_WRITE(buffer);
	}

	FILE_CLOSE();
}

/****************************************************************************
*	vTracealyzerAssignUserEventAsExtendedData
*	Call this function to assign an user event as extended data.
*	This will allow manual interrupt starts and ends to be flagged using
*	vTracealyzerInterruptStart and vTracealyzerInterruptEnd since
*	tInterruptStart and tInterruptEnd is no longer used by On Time RTOS-32.
****************************************************************************/
void vTracealyzerAssignUserEventAsExtendedData(RTKTraceEvent tUserEvent)
{
	assert(extendedDataEvent == 0);
	extendedDataEvent = tUserEvent;
}

/****************************************************************************
*	vTracealyzerInterruptStart
*	Call this function to indicate the start of an interrupt.
*	NOTE: A user event must first be assigned as extended data using
*	vTracealyzerSetUserEventAsExtendedData().
****************************************************************************/
void vTracealyzerExtendedInterruptStart()
{
	assert(extendedDataEvent != 0);
	RTKUserTrace(extendedDataEvent, EXTENDED_INTERRUPT_START);
}

/****************************************************************************
*	vTracealyzerInterruptEnd
*	Call this function to indicate the end of an interrupt.
*	NOTE: A user event must first be assigned as extended data using
*	vTracealyzerSetUserEventAsExtendedData().
****************************************************************************/
void vTracealyzerExtendedInterruptEnd()
{
	assert(extendedDataEvent != 0);
	RTKUserTrace(extendedDataEvent, EXTENDED_INTERRUPT_END);
}

/****************************************************************************
*	vTracealyzerExport
*	Entry function that will export everything in the RTKTraceBuffer to an
*	XML format used by Tracealyzer.
*	NOTE: The trace data collection should be stopped before this using
*	RTKStopTracing!
****************************************************************************/
void vTracealyzerExport(const char* szFileName)
{
	int i, j;

	FILE_OPEN(szFileName);
	FILE_WRITE("<?xml version=\"1.0\" encoding=\"" TRACE_XML_ENCODING "\"?>\r\n");
	FILE_WRITE("<XmlTrace>\r\n");

#ifdef TRACE_USING_MULTIPROCESSOR_KERNEL
	CPUs = RTKCPUs();
	if (CPUs > TRACE_MAX_CPUS)
	{
		FILE_WRITE("<TraceError "TRACE_XML_string"=\"RTKCPUs() returned a higher value than TRACE_MAX_CPUS!\" />\r\n");
		FILE_WRITE("</XmlTrace>\r\n");
		FILE_CLOSE();
		return;
	}
#endif

	// Reset the cpuContext struct
	for (i = 0; i < CPUs; i++)
	{
		cpuContext[i].currentSourceObjectIndex = -1;
		cpuContext[i].interruptStackIndex = 0;
		cpuContext[i].pPreviousEvent = 0;
		cpuContext[i].previousLevel = 0;
		for (j = 0; j < MAX_STACK_SIZE; j++)
		{
			cpuContext[i].interruptStack[j] = 0;
		}
	}

	// Reset timestamp handling
	timestampPadding = 0;
	previousTimestamp = 0;

	if (RTKConfig.Flags & RF_TRACE_TIME_US)
		interruptContextSwitchTime = TRACE_INTERRUPT_CONTEXT_SWITCH_TIME / 1000;

	if (NUM_EVENTS > VALID_TO_MASK)
	{
		FILE_WRITE("<TraceError "TRACE_XML_string"=\"This error should never happen! IS_SEMAPHORE_EVENT_BIT is too low or the number of events in the buffer is reported as way too large.\" />\r\n");
		FILE_WRITE("</XmlTrace>\r\n");
		FILE_CLOSE();
		return;
	}

	if (NUM_EVENTS > 1)
	{
		if (!prvGetTraceRecord(0)->TimeStamp && !prvGetTraceRecord(1)->TimeStamp)
		{
			FILE_WRITE("<TraceError "TRACE_XML_string"=\"Add \r\n if (RTKDebugVersion()) RTKConfig.Flags |= RF_ICPUTIME | RF_TRACE_TIME_NS; \r\n in main() function, to activate the "TRACE_XML_timestamp" functionality.\" />\r\n");
			FILE_WRITE("</XmlTrace>\r\n");
			FILE_CLOSE();
		}
	}

	prvExportServicesAndObjects();

	prvExportEvents();

	FILE_WRITE("</XmlTrace>\r\n");

	FILE_CLOSE();
}

/****************************************************************************
*	prvExportServicesAndObjects
*	This function writes the service information and then calls
*	prvExportObjectTable that exports all objects used in the trace.
****************************************************************************/
void prvExportServicesAndObjects()
{
	char buffer[MAX_LINE_LENGTH];
	FILE_WRITE("\t<"TRACE_XML_TraceInformation">\r\n");
	FILE_WRITE("\t\t<"TRACE_XML_ServiceTable">\r\n");

	FILE_WRITE_SERVICE(RTKUnknown);
	FILE_WRITE_SERVICE(RTKWait);
	FILE_WRITE_SERVICE(RTKSignal);
	FILE_WRITE_SERVICE(RTKPut);
	FILE_WRITE_SERVICE(RTKGet);
	FILE_WRITE_SERVICE(RTKSend);
	FILE_WRITE_SERVICE(RTKReceive);
	FILE_WRITE_SERVICE(RTKLockSpinlock);
	FILE_WRITE_SERVICE(RTKReleaseSpinlock);
	FILE_WRITE_SERVICE(RTKSetPriority);
	FILE_WRITE_SERVICE(RTKDelayUntil);
	FILE_WRITE_SERVICE(RTKDeleteSemaphore);
	FILE_WRITE_SERVICE(RTKDeleteSpinlock);
	FILE_WRITE_SERVICE(RTKDeleteMailbox);
	FILE_WRITE_SERVICE(RTKPulse);
	FILE_WRITE_SERVICE(RTKResetEvent);

	FILE_WRITE("\t\t</"TRACE_XML_ServiceTable">\r\n");
	FILE_WRITE("\t\t<"TRACE_XML_TargetPlatform">On Time RTOS-32</"TRACE_XML_TargetPlatform">\r\n");

#ifdef TRACE_SEQUENTIAL_TIMESTAMP_MODE
	// Set frequency to 0 for sequential mode
	sprintf(buffer, "\t\t<"TRACE_XML_TickFrequency">%lu</"TRACE_XML_TickFrequency">\r\n", 0);
#else
	if (RTKConfig.Flags & RF_TRACE_TIME_NS)
		sprintf(buffer, "\t\t<"TRACE_XML_TickFrequency">%lu</"TRACE_XML_TickFrequency">\r\n", 1000000000);
	else if (RTKConfig.Flags & RF_TRACE_TIME_US)
		sprintf(buffer, "\t\t<"TRACE_XML_TickFrequency">%lu</"TRACE_XML_TickFrequency">\r\n", 1000000);
#endif // TRACE_SEQUENTIAL_TIMESTAMP_MODE
	FILE_WRITE(buffer);

	sprintf(buffer, "\t\t<"TRACE_XML_NumCpus">%d</"TRACE_XML_NumCpus">\r\n", CPUs);
	FILE_WRITE(buffer);

	prvExportObjectTable();

	FILE_WRITE("\t</"TRACE_XML_TraceInformation">\r\n");
};

/****************************************************************************
*	prvExportObjectTable
*	This function loops through the trace buffer and searches for any
*	objects used by the system. All objects will be stored in "objects"
*	and index + 1 is the object code used in the XML file.
****************************************************************************/
void prvExportObjectTable()
{
	char buffer[MAX_LINE_LENGTH];
	char buffer2[MAX_LINE_LENGTH];
	struct rtkTraceRec* pCurrentEvent;
	int i, objectIndex;
#if RTT32_VER < 530
	unsigned int hasWarnedDelete = 0, hasWarnedEvent = 0;
#endif

#ifdef TRACE_USING_MULTIPROCESSOR_KERNEL
	if (CPUs == 0)
	{
		FILE_WRITE("<TraceError "TRACE_XML_string"=\"RTKCPUs() returned 0. The configuration is invalid.\" />\r\n");
		return;
	}
#endif

	objectCounter = 0;

	FILE_WRITE("\t\t<"TRACE_XML_ObjectTable">\r\n");

	// We add an idle task for each core
	for (i = 0; i < CPUs; i++)
	{
		objects[objectCounter].state = tNewInstance;
		objects[objectCounter].object = 0;
		objectCounter++;
		if (RTKDebugVersion() == 0)
			sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Unknown Task %d\" "TRACE_XML_class"=\"Task\" "TRACE_XML_priority"=\"%d\"/>\r\n", objectCounter, i, 0); // In Release Mode; add Unkown Task X (missing proper context switches).
		else
			sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Idle Task %d\" "TRACE_XML_class"=\"Task\" "TRACE_XML_priority"=\"%d\"/>\r\n", objectCounter, i, 0); // In Debug Mode; add Idle Task X
		FILE_WRITE(buffer);
	}

	for (i = NUM_EVENTS; i >= 0; i--)
	{
		pCurrentEvent = prvGetTraceRecord(i);

		if (pCurrentEvent->Event == tNoEvent)
			continue;

		// Check the source of this event
		if (prvGetTaskIndex((unsigned int)pCurrentEvent->Source, i, pCurrentEvent) < 0)
		{
			// Not already added
			if (objectCounter < TRACE_MAX_OBJECTS)
			{
				// There is room for it
				if ((unsigned int)pCurrentEvent->Source <= RTKMAXIRQS)
				{
					// The source is an interrupt
					objects[objectCounter].state = tInterruptEnd;
					objects[objectCounter].object = (unsigned int)pCurrentEvent->Source;
					objectCounter++;

					sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"IRQ%d\" "TRACE_XML_class"=\"ISR\" />\r\n", objectCounter, (int)pCurrentEvent->Source);
					FILE_WRITE(buffer);
				}
				else
				{
					// Don't add the idle tasks
					if (pCurrentEvent->Source->Prio > 0)
					{
						// The source is a task, and not an idle task
						objects[objectCounter].state = tNewInstance;
						objects[objectCounter].object = (unsigned int)pCurrentEvent->Source;
						objectCounter++;

						if (pCurrentEvent->Source->TCBId == LegalTCBId)
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"%s\" "TRACE_XML_class"=\"Task\" "TRACE_XML_priority"=\"%d\"/>\r\n", objectCounter, pCurrentEvent->Source->Name, pCurrentEvent->Source->Prio);
						else
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Thread @ 0x%08X\" "TRACE_XML_class"=\"Task\" "TRACE_XML_priority"=\"%d\"/>\r\n", objectCounter, (unsigned int)pCurrentEvent->Source, 0); /* TODO: Set unknown priority */
						FILE_WRITE(buffer);
					}
				}
			}
			else
			{
				objectCounter++;
			}
		}

		// Check the parameter for this event
		switch (RTKTraceParameterOf[pCurrentEvent->Event])
		{
		case TP_NONE:
			break;
		case TP_TASK:
			// Task
			if (pCurrentEvent->P.Task != RTK_NO_TASK)
			{
				// An OK task
				if (prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, i, pCurrentEvent) < 0)
				{
					// Not already added
					if (objectCounter < TRACE_MAX_OBJECTS)
					{
						// There is room for it
						objects[objectCounter].state = tNewInstance;
						objects[objectCounter].object = (unsigned int)pCurrentEvent->P.Task;
						objectCounter++;

						if (pCurrentEvent->P.Task->TCBId == LegalTCBId)
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"%s\" "TRACE_XML_class"=\"Task\" "TRACE_XML_priority"=\"%d\"/>\r\n", objectCounter, pCurrentEvent->P.Task->Name, pCurrentEvent->P.Task->Prio);
						else
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Thread @ 0x%08X\" "TRACE_XML_class"=\"Task\" "TRACE_XML_priority"=\"%d\"/>\r\n", objectCounter, (unsigned int)pCurrentEvent->P.Task, 0); /* TODO: Set unknown priority */
						FILE_WRITE(buffer);
					}
					else
					{
						objectCounter++;
					}
				}
			}
			break;
		case TP_SEMA:
			// Semaphore
			if (pCurrentEvent->P.Sema != NULL)
			{
				// An OK semaphore
				if (prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, i) < 0)
				{
					// Not already added
					if (objectCounter < TRACE_MAX_OBJECTS)
					{
						// There is room for it
						objects[objectCounter].object = (unsigned int)pCurrentEvent->P.Sema;
						objectCounter++;

						if (prvIsSemaphoreEvent(objectCounter - 1, i))
						{
							objects[objectCounter - 1].validToAndIsEvent = IS_SEMAPHORE_EVENT_MASK;
#if RTT32_VER < 530
							if (!hasWarnedEvent)
							{
								FILE_WRITE("<TraceWarning "TRACE_XML_string"=\"Correct tracing of semaphores of type ST_EVENT requires On Time RTOS-32 v5.30 or later.\" />\r\n");
								hasWarnedEvent = 1;
							}
#endif
						}

						if (pCurrentEvent->P.Sema->SemaId == LegalSemaId)
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"%s\" "TRACE_XML_class"=\"%s\" />\r\n", objectCounter, pCurrentEvent->P.Sema->Name, objects[objectCounter - 1].validToAndIsEvent & IS_SEMAPHORE_EVENT_MASK ? "Semaphore(ST_EVENT)" : "Semaphore");
						else
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Semaphore @ 0x%08X\" "TRACE_XML_class"=\"%s\" />\r\n", objectCounter, (unsigned int)pCurrentEvent->P.Sema, objects[objectCounter - 1].validToAndIsEvent & IS_SEMAPHORE_EVENT_MASK ? "Semaphore(ST_EVENT)" : "Semaphore");
						FILE_WRITE(buffer);
					}
					else
					{
						objectCounter++;
					}
				}
			}
			break;
		case TP_MBOX:
			// Mailbox
			if (pCurrentEvent->P.MBox != NULL)
			{
				// An OK semaphore
				if (prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, i) < 0)
				{
					// Not already added
					if (objectCounter < TRACE_MAX_OBJECTS)
					{
						// There is room for it
						objects[objectCounter].object = (unsigned int)pCurrentEvent->P.MBox;
						objectCounter++;

						if (pCurrentEvent->P.MBox->MCBId == LegalMCBId)
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"%s\" "TRACE_XML_class"=\"MBox\" />\r\n", objectCounter, pCurrentEvent->P.MBox->Name);
						else
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Mailbox @ 0x%08X\" "TRACE_XML_class"=\"MBox\" />\r\n", objectCounter, (unsigned int)pCurrentEvent->P.MBox);
						FILE_WRITE(buffer);
					}
					else
					{
						objectCounter++;
					}
				}
			}
			break;
		case TP_LOCK:
			// Spinlock
			if (pCurrentEvent->P.Lock != NULL)
			{
				if (prvGetObjectIndex((unsigned int)pCurrentEvent->P.Lock, i) < 0)
				{
					// Not already added
					if (objectCounter < TRACE_MAX_OBJECTS)
					{
						// There is room for it
						objects[objectCounter].object = (unsigned int)pCurrentEvent->P.Lock;
						objectCounter++;

						if (prvIsValidName(pCurrentEvent->P.Lock->Name))
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"%s\" "TRACE_XML_class"=\"Lock\" />\r\n", objectCounter, pCurrentEvent->P.Lock->Name);
						else
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Spinlock @ 0x%08X\" "TRACE_XML_class"=\"Lock\" />\r\n", objectCounter, (unsigned int)pCurrentEvent->P.Lock);
						FILE_WRITE(buffer);
					}
					else
					{
						objectCounter++;
					}
				}
			}
			break;
		case TP_PTR:
			// Deletes
#if RTT32_VER >= 528
			if (pCurrentEvent->P.Ptr != NULL)
			{
				objectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Ptr, i);
				if (objectIndex < 0)
				{
			// Not already added
					if (objectCounter < TRACE_MAX_OBJECTS)
					{
			// There is room for it
						objects[objectCounter].validToAndIsEvent = i;
						objects[objectCounter].object = (unsigned int)pCurrentEvent->P.Ptr;
						objectCounter++;
						
						switch (pCurrentEvent->Event)
						{
						case tDeleteThread:
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Thread @ 0x%08X\" "TRACE_XML_class"=\"Task\" />\r\n", objectCounter, pCurrentEvent->P.Ptr);
							break;
						case tDeleteSemaphore:
#if RTT32_VER < 530
							if (!hasWarnedDelete)
							{
								FILE_WRITE("<TraceWarning "TRACE_XML_string"=\"Correct tracing of RTKDeleteSemaphore requires On Time RTOS-32 v5.30 or later.\" />\r\n");
								hasWarnedDelete = 1;
							}
#endif // RTT32_VER < 530

							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Semaphore @ 0x%08X\" "TRACE_XML_class"=\"Semaphore\" />\r\n", objectCounter, pCurrentEvent->P.Ptr);
							break;
						case tDeleteSpinlock:
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Spinlock @ 0x%08X\" "TRACE_XML_class"=\"Lock\" />\r\n", objectCounter, pCurrentEvent->P.Ptr);
							break;
						case tDeleteMBox:
							sprintf(buffer, "\t\t\t<"TRACE_XML_Object" "TRACE_XML_code"=\"%u\" "TRACE_XML_name"=\"Mailbox @ 0x%08X\" "TRACE_XML_class"=\"MBox\" />\r\n", objectCounter, pCurrentEvent->P.Ptr);
							break;
						default:
							assert(0);
							break;
						}
						FILE_WRITE(buffer);
					}
					else
					{
						objectCounter++;
					}
				}
				else
				{
			// Set validToAndIsEvent
					objects[objectIndex].validToAndIsEvent |= i;
				}
			}
#endif // RTT32_VER >= 528
			break;
		case TP_NUM:
			break;
		case TP_STR:
			break;
		default:
			break;
		}
	}

	FILE_WRITE("\t\t</"TRACE_XML_ObjectTable">\r\n");

	if (objectCounter > TRACE_MAX_OBJECTS)
	{
		// We have tried to add more objects than we have room for
		FILE_WRITE("<TraceWarning "TRACE_XML_string"=\"Objects are missing in the trace!\r\nTRACE_MAX_OBJECTS (trcConfig.h) must be increased.\" />\r\n");
	}
}

/****************************************************************************
*	prvExportEvents
*	This function loops through the trace buffer and outputs the
*	necessary events to the XML file.
****************************************************************************/
void prvExportEvents()
{
	char buffer[MAX_LINE_LENGTH];
	struct rtkTraceRec *pCurrentEvent, *pOtherEvent;
	int i, result, level, sourceObjectIndex, referencedObjectIndex;

	FILE_WRITE("\t<"TRACE_XML_Events">\r\n");

	// First look for the first proper task context. We might have lots of interrupts in the beginning of the trace, so we loop until we find what those interrupts interrupted
	for (i = 0; i < CPUs; i++)
	{
		for (level = NUM_EVENTS; level >= 0; level--)
		{
			pCurrentEvent = prvGetTraceRecord(level);

			if (pCurrentEvent->Event != tNoEvent && (unsigned int)pCurrentEvent->Source > RTKMAXIRQS && prvGetCPUFromEvent(pCurrentEvent) == i)
			{
				// We found the first task executing on this CPU
				cpuContext[i].currentSourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
				break;
			}
		}
		if (cpuContext[i].currentSourceObjectIndex == -1)
		{
			// No CPU activity found
			cpuContext[i].currentSourceObjectIndex = i;
		}
	}

	// We have to find running tasks for the trace to make sense (but we don't check for it)
	// assert(currentSourceObjectIndex != -1);

	// Continue /*from level*/ with the rest of the events
	for (level = NUM_EVENTS; level >= 0; level--)
	{
		pCurrentEvent = prvGetTraceRecord(level);

		// Check if we have handled any events on this CPU before
		if (cpuContext[prvGetCPUFromEvent(pCurrentEvent)].pPreviousEvent == 0)
		{
			// First event on this CPU
			if ((unsigned int)pCurrentEvent->Source > RTKMAXIRQS && pCurrentEvent->Event != tNoEvent && pCurrentEvent->Event != tStateCurrent)
			{
				// Not an interrupt, not an invalid event and not already a context switch event that will be handled later anyway

				// Add a context switch event for the first proper task that was detected on for this CPU
				sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), cpuContext[prvGetCPUFromEvent(pCurrentEvent)].currentSourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
			}
		}

		// Handle interrupts
		if ((unsigned int)pCurrentEvent->Source < RTKMAXIRQS)
		{
			// Current event is in an interrupt

			// Check previous event on this CPU
			pOtherEvent = cpuContext[prvGetCPUFromEvent(pCurrentEvent)].pPreviousEvent;
			if (level == NUM_EVENTS || pOtherEvent == 0 || pOtherEvent->Event == tNoEvent)
			{
				// No previous event, meaning first actual event is in an interrupt
				assert(cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStackIndex < MAX_STACK_SIZE);
				cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStack[cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStackIndex++] = cpuContext[prvGetCPUFromEvent(pCurrentEvent)].currentSourceObjectIndex;

				sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

				// Add a context switch to this interrupt
				sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));

				FILE_WRITE(buffer);
			}
			else
			{
				// Previous event is a proper event
				sourceObjectIndex = prvGetTaskIndex((unsigned int)pOtherEvent->Source, cpuContext[prvGetCPUFromEvent(pOtherEvent)].previousLevel, pOtherEvent);
				if ((unsigned int)pOtherEvent->Source < RTKMAXIRQS)
				{
					// Previous event is also an interrupt
					if (pOtherEvent->Source == pCurrentEvent->Source)
					{
						// Same interrupt source

						result = 0;
						if (pCurrentEvent->Event == extendedDataEvent && (pCurrentEvent->P.Num & EXTENDED_TYPE_MASK) == EXTENDED_INTERRUPT_START)
						{
							// Current event is an extended interrupt start, so it is starting a new instance

							result = 1;
						}
						else if (pOtherEvent->Event == extendedDataEvent && (pOtherEvent->P.Num & EXTENDED_TYPE_MASK) == EXTENDED_INTERRUPT_END)
						{
							// Previous event is an extended interrupt end, so it ended its instance

							result = 1;
						}
						else if (objects[sourceObjectIndex].state != tExtendedInterruptStart && prvGetTimestampDiff(pCurrentEvent, pOtherEvent) > interruptContextSwitchTime)
						{
							// This is not an extended data interrupt and it is too long between timestamps to be considered the same instance

							result = 1;
						}

						if (result)
						{
							// Switch to whatever is on the stack

							sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"0\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pOtherEvent), cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStack[cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStackIndex - 1] + 1, prvGetCPUFromEvent(pOtherEvent));
							FILE_WRITE(buffer);

							// Switch back to current
							sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

							sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
							FILE_WRITE(buffer);
						}
					}
					else
					{
						// Different interrupt sources
						result = 1;
						if (objects[sourceObjectIndex].state == tExtendedInterruptStart)
						{
							// Previous event is started using extended data
							assert(cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStackIndex < MAX_STACK_SIZE);
							cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStack[cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStackIndex++] = sourceObjectIndex;

							// Switch to the current interrupt
							sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

							sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
							FILE_WRITE(buffer);
						}
						else
						{
							// Previous event is not started using extended data

							// Check current event
							sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
							if (objects[sourceObjectIndex].state == tExtendedInterruptStart)
							{
								// Current event is started using extended data
								assert(cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStackIndex > 0);
								cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStack[--cpuContext[prvGetCPUFromEvent(pCurrentEvent)].interruptStackIndex] = 0;

								// Switch to the current interrupt
								sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

								sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"0\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pOtherEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
								FILE_WRITE(buffer);
							}
							else
							{
								// Neither event is started using extended data

								// Switch to whatever is on the stack
								sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"0\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pOtherEvent), cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStack[cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStackIndex - 1] + 1, prvGetCPUFromEvent(pOtherEvent));
								FILE_WRITE(buffer);

								// Switch to the current interrupt
								sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

								sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
								FILE_WRITE(buffer);
							}
						}
					}
				}
				else
				{
					// Previous event is not an interrupt
					assert(cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStackIndex < MAX_STACK_SIZE);
					cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStack[cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStackIndex++] = cpuContext[prvGetCPUFromEvent(pOtherEvent)].currentSourceObjectIndex;

					// Switch to the current interrupt
					sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

					sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);
				}
			}
		}
		else
		{
			// Current event is not an interrupt
			if (level < NUM_EVENTS)
			{
				// Check previous event
				pOtherEvent = cpuContext[prvGetCPUFromEvent(pCurrentEvent)].pPreviousEvent;
				if (pOtherEvent != 0 && pOtherEvent->Event != tNoEvent && (unsigned int)pOtherEvent->Source < RTKMAXIRQS)
				{
					// Previous event is an interrupt
					assert(cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStackIndex > 0);
					cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStack[--cpuContext[prvGetCPUFromEvent(pOtherEvent)].interruptStackIndex] = 0;

					// Switch to the current object
					sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

					sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"0\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pOtherEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pOtherEvent));
					FILE_WRITE(buffer);
				}
			}
		}

		switch (pCurrentEvent->Event)
		{
		case tNoEvent:
			break;
		case tStateReady:
			// Determine if this task should be flagged as a new instance
			referencedObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);

			sprintf(buffer, "\t\t<"TRACE_XML_ActorReadyEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			if (objects[referencedObjectIndex].state == 0) // Do not overwrite any previous states
				if (pCurrentEvent->Source != pCurrentEvent->P.Task)
					objects[referencedObjectIndex].state = tNewInstance; // Another task flags this task as ready; the next time this task runs it is a new instance
			break;
		case tNewTime:
			sprintf(buffer, "\t\t<"TRACE_XML_KernelNoticeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_string"=\"NewTime(%d)\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), pCurrentEvent->P.Num, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tStateDeadlocked:
		case tStateIllegal:
		case tStateTerminated:
		case tSchedOff:
		case tSchedOn:
		case tTimeSlice:
			// Ignore these
			break;
		case tSendIPI:
			sprintf(buffer, "\t\t<"TRACE_XML_KernelNoticeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_string"=\"Send IPI (%d)\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), pCurrentEvent->P.Num, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tReceiveIPI:
			sprintf(buffer, "\t\t<"TRACE_XML_KernelNoticeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_string"=\"Receive IPI (%d)\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), pCurrentEvent->P.Num, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tStateCurrent:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);
			if (objects[sourceObjectIndex].state == 0)
			{
				sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"0\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
			}
			else
			{
				// pCurrentEvent is a new instance, and possibly something more
				sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);

				// Check if it is something more
				switch (objects[sourceObjectIndex].state)
				{
				case tNewInstance:
					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateDelaying:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_Timeout], RTKDelayUntil, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateTimedWait:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_Timeout], RTKWait, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateTimedPut:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_Timeout], RTKPut, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateTimedGet:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_Timeout], RTKGet, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateTimedSend:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_Timeout], RTKSend, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateTimedReceive:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_Timeout], RTKReceive, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateBlockedSend:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_ReturnSuccess], RTKSend, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateBlockedReceive:
					sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_ReturnSuccess], RTKReceive, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);

					// Reset object state
					objects[sourceObjectIndex].state = 0;
					break;
				case tStateBlockedWait:
					// Special handling of semaphore wait
					if (level > 0)
					{
						result = 0;

						pOtherEvent = prvGetTraceRecord(level - 1);
						if (pOtherEvent->Event != tSemaDec)
						{
							// Semaphore of type ST_EVENT, all others must have tSemaDec
							result = 1;
						}
						else
						{
							// A tSemaDec event, but is it a normal one?
							referencedObjectIndex = prvGetObjectIndex((unsigned int)pOtherEvent->P.Sema, level - 1);
							if (objects[sourceObjectIndex].refObject != referencedObjectIndex)
							{
								// Semaphore of type ST_EVENT, all others should have the same reference
								result = 1;
							}
						}

						if (result)
						{
							// We are dealing with a semaphore of type ST_EVENT
							sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), results[RESULT_ReturnSuccess], RTKWait, prvGetCPUFromEvent(pCurrentEvent));
							FILE_WRITE(buffer);

							// Reset object state
							objects[sourceObjectIndex].state = 0;
						}
					}
					break;
				}
			}
			// Store the current object's code
			cpuContext[prvGetCPUFromEvent(pCurrentEvent)].currentSourceObjectIndex = sourceObjectIndex;
			break;
		case tInterruptStart:
			// NOTE: This event is not used in On Time RTOS-32 any longer!
			/*
			referencedObjectIndex = prvGetObjectCode((unsigned int)pCurrentEvent->P.Num);
			sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1);
			FILE_WRITE(buffer);*/
			break;
		case tInterruptEnd:
			// NOTE: This event is not used in On Time RTOS-32 any longer!
			/*
			sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"0\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), currentObjectCode + 1);
			FILE_WRITE(buffer);*/
			break;
		case tPriorityDropped:
			referencedObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);
			sprintf(buffer, "\t\t<"TRACE_XML_PriorityChangeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_direction"=\"Lowered\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tPriorityRaised:
			referencedObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);
			sprintf(buffer, "\t\t<"TRACE_XML_PriorityChangeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_direction"=\"Raised\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tStateDelaying:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tStateDelaying;

			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_numericParameter"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), pCurrentEvent->P.Num, results[RESULT_Block], RTKDelayUntil, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tTimeout:
			referencedObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);

			if (pCurrentEvent->P.Task->TCBId == LegalTCBId)
			{
				sprintf(buffer, "\t\t<"TRACE_XML_KernelNoticeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_string"=\"Timeout(%s)\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), pCurrentEvent->P.Task->Name, prvGetCPUFromEvent(pCurrentEvent));
			}
			else
			{
				sprintf(buffer, "\t\t<"TRACE_XML_KernelNoticeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_string"=\"Timeout(Thread @ 0x%08X)\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), (unsigned int)pCurrentEvent->P.Task, prvGetCPUFromEvent(pCurrentEvent));
			}
			FILE_WRITE(buffer);

			// Now we flag the task as having timed out
			switch (objects[referencedObjectIndex].state)
			{
			case tStateDelaying:
				// Do nothing
				break;
			case tStateBlockedWait:
				objects[referencedObjectIndex].state = tStateTimedWait;
				break;
			case tStateBlockedPut:
				objects[referencedObjectIndex].state = tStateTimedPut;
				break;
			case tStateBlockedGet:
				objects[referencedObjectIndex].state = tStateTimedGet;
				break;
			case tStateBlockedSend:
				objects[referencedObjectIndex].state = tStateTimedSend;
				break;
			case tStateBlockedReceive:
				objects[referencedObjectIndex].state = tStateTimedReceive;
				break;
			}
			break;
		case tUserEvent_1:
		case tUserEvent_2:
		case tUserEvent_3:
		case tUserEvent_4:
		case tUserEvent_5:
		case tUserEvent_6:
		case tUserEvent_7:
		case tUserEvent_8:
		case tUserEvent_9:
		case tUserEvent_10:
			if (pCurrentEvent->Event == extendedDataEvent)
			{
				switch (pCurrentEvent->P.Num & EXTENDED_TYPE_MASK)
				{
				case EXTENDED_INTERRUPT_START:
					sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
					objects[sourceObjectIndex].state = tExtendedInterruptStart;
					break;
				case EXTENDED_INTERRUPT_END:
					sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
					objects[sourceObjectIndex].state = tExtendedInterruptEnd;
					break;
				}
			}
			else
			{
				if (RTKDebugVersion() == 0)
				{
					// We're in release mode. Release mode lacks context switches, so we add manual context switch to current task.
					sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
					sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);
				}

				sprintf(buffer, "\t\t<"TRACE_XML_UserEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_value"=\"%d\" "TRACE_XML_channel"=\"%s\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), pCurrentEvent->P.Num, RTKTraceName[pCurrentEvent->Event], prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);

				if (RTKDebugVersion() == 0)
				{
					// We're in release mode. Release mode lacks context switches, so we add manual context switch to previous task.
					sprintf(buffer, "\t\t<"TRACE_XML_ContextSwitchEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_actor"=\"%d\" "TRACE_XML_newInstance"=\"1\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), cpuContext[prvGetCPUFromEvent(pCurrentEvent)].currentSourceObjectIndex + 1, prvGetCPUFromEvent(pCurrentEvent));
					FILE_WRITE(buffer);
				}
			}
			break;
		case tStateBlockedSend:
		case tStateTimedSend:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tStateBlockedSend;

			referencedObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_Block], RTKSend, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tStateBlockedReceive:
		case tStateTimedReceive:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tStateBlockedReceive;

			// TODO: Check if this works... Shouldn't ref be source?
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, results[RESULT_Block], RTKReceive, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tMessageSend:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

			referencedObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->P.Task, level, pCurrentEvent);
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKSend, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tMessageReceive:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

			// InstantSuccess for message receive must reference itself
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), sourceObjectIndex + 1, results[RESULT_InstantSuccess], RTKReceive, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
#if RTT32_VER >= 530
		case tSemaPulse:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
			
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKPulse, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tSemaReset:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
			
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKResetEvent, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
#endif // RTT32_VER >= 530
		case tStateBlockedWait:
		case tStateTimedWait:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tStateBlockedWait;

			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
			// Store the reference
			objects[sourceObjectIndex].refObject = referencedObjectIndex;

			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_Block], RTKWait, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tSemaInc:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);

			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKSignal, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tSemaDec:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

			switch (objects[sourceObjectIndex].state)
			{
			case tStateBlockedWait:
				// Reset object state
				objects[sourceObjectIndex].state = 0;

				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_ReturnSuccess], RTKWait, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			default:
				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKWait, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			}
			break;
		case tStateBlockedPut:
		case tStateTimedPut:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tStateBlockedPut;

			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, level);
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_Block], RTKPut, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tStateBlockedGet:
		case tStateTimedGet:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tStateBlockedGet;

			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, level);
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_Block], RTKGet, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tMBIn:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

			switch (objects[sourceObjectIndex].state)
			{
			case tStateBlockedPut:
				// Reset object state
				objects[sourceObjectIndex].state = 0;

				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_ReturnSuccess], RTKPut, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			default:
				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKPut, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			}
			break;
		case tMBOut:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

			switch (objects[sourceObjectIndex].state)
			{
			case tStateBlockedGet:
				// Reset object state
				objects[sourceObjectIndex].state = 0;

				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_ReturnSuccess], RTKGet, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			default:
				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.MBox, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKGet, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			}
			break;
		case tWaitLock:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);
			objects[sourceObjectIndex].state = tWaitLock;

			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Lock, level);
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_Block], RTKLockSpinlock, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tReleaseLock:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Lock, level);

			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKReleaseSpinlock, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tLockLock:
			sourceObjectIndex = prvGetTaskIndex((unsigned int)pCurrentEvent->Source, level, pCurrentEvent);

			switch (objects[sourceObjectIndex].state)
			{
			case tWaitLock:
				// Reset object state
				objects[sourceObjectIndex].state = 0;

				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Lock, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_ReturnSuccess], RTKLockSpinlock, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			default:
				referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Lock, level);
				sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKLockSpinlock, prvGetCPUFromEvent(pCurrentEvent));
				FILE_WRITE(buffer);
				break;
			}
			break;
#if RTT32_VER >= 528
		case tDeleteThread:
			sprintf(buffer, "\t\t<"TRACE_XML_KernelNoticeEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_string"=\"Deallocated terminated thread(Thread @ 0x%08X)\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), (unsigned int)pCurrentEvent->P.Task, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tDeleteSemaphore:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
			
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKDeleteSemaphore, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tDeleteMBox:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
			
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKDeleteMailbox, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
		case tDeleteSpinlock:
			referencedObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, level);
			
			sprintf(buffer, "\t\t<"TRACE_XML_KernelServiceEvent" "TRACE_XML_timestamp"=\"%llu\" "TRACE_XML_referencedObject"=\"%d\" "TRACE_XML_result"=\"%s\" "TRACE_XML_service"=\"%d\" "TRACE_XML_CPU"=\"%d\" />\r\n", prvGetTimestampFromEvent(pCurrentEvent), referencedObjectIndex + 1, results[RESULT_InstantSuccess], RTKDeleteSpinlock, prvGetCPUFromEvent(pCurrentEvent));
			FILE_WRITE(buffer);
			break;
#endif // RTT32_VER >= 528
		default:
			//assert(0);
			break;
		}

		// Store the current event before processing next event
		cpuContext[prvGetCPUFromEvent(pCurrentEvent)].pPreviousEvent = pCurrentEvent;
		cpuContext[prvGetCPUFromEvent(pCurrentEvent)].previousLevel = level;
	}

	FILE_WRITE("\t</"TRACE_XML_Events">\r\n");
}

/****************************************************************************
*	prvGetTaskIndex
*	This function will return the XML object code for the requested task.
****************************************************************************/
int prvGetTaskIndex(unsigned int handle, unsigned int level, struct rtkTraceRec* rec)
{
	// Make sure it's not an interrupt
	if (handle > RTKMAXIRQS)
	{
		// This is a task, not an interrupt
		if (((RTKTaskHandle)handle)->Prio == 0)
		{
			// This is an idle task, so we return the idle task that we want to visualize for that CPU (located on index 0..CPUs-1)
			return prvGetCPUFromEvent(rec);
		}
	}

	return prvGetObjectIndex(handle, level);
}

/****************************************************************************
*	prvGetObjectIndex
*	This function will return the XML object code for the requested object.
****************************************************************************/
int prvGetObjectIndex(unsigned int object, unsigned int level)
{
	int i;
	for (i = CPUs; i < objectCounter; i++)
		if (objects[i].object == object && (objects[i].validToAndIsEvent & VALID_TO_MASK) <= level)
			return i;
	return -1;
}

/****************************************************************************
*	prvGetTraceRecord
*	This function will return the requested trace record.
****************************************************************************/
struct rtkTraceRec* prvGetTraceRecord(int level)
{
	return RTKTraceBuffer->Events + ((RTKTraceBuffer->BufferEnd - level) & RTKTraceBuffer->BufferSizeMask);
}

/****************************************************************************
*	prvIsSemaphoreEvent
*	This function will attempt to determine if a semaphore is of type
*	ST_EVENT or not, based on usage. 
****************************************************************************/
unsigned int prvIsSemaphoreEvent(int objectIndex, int level)
{
	int i, currentObjectIndex, blockedSourceObjectIndex = -11;
	unsigned int foundBlock = 0;
	struct rtkTraceRec *pCurrentEvent, *pOtherEvent;

	for (i = level; i >= 0; i--)
	{
		pCurrentEvent = prvGetTraceRecord(i);

		if (foundBlock)
		{
			// We have previously found a block
			if (pCurrentEvent->Event == tStateCurrent)
			{
				// We are switching to a task
				currentObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Task, i);
				if (currentObjectIndex == blockedSourceObjectIndex)
				{
					// We are switching to the blocked task we're looking for
					if (i > 0)
					{
						// We may look ahead
						pOtherEvent = prvGetTraceRecord(i - 1);

						if (pOtherEvent->Event == tSemaDec)
						{
							// Semaphore decrease
							currentObjectIndex = prvGetObjectIndex((unsigned int)pOtherEvent->P.Sema, i - 1);
							if (currentObjectIndex == objectIndex)
							{
								// The task was unblocked by a semaphore decrease on the right object, so this is not an event semaphore
								return 0;
							}
							else
							{
								// Semaphore decrease was called on some other object, this is an event semaphore
								return 1;
							}
						}
						else
						{
							// It is not a semaphore decrease, therefore it is an event semaphore
							return 1;
						}
					}
					else
					{
						// We can't determine if this is an event semaphore
						return 0;
					}
				}
			}
			else if (pCurrentEvent->Event == tTimeout)
			{
				// A timeout has occurred
				currentObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Task, i);

				if (currentObjectIndex == blockedSourceObjectIndex)
				{
					// Timeout on the task that was blocked
					foundBlock = 0;
				}
			}
		}
		else if (RTKTraceParameterOf[pCurrentEvent->Event] == TP_SEMA)
		{
			// The event handles a semaphore
			currentObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->P.Sema, i);
			if (currentObjectIndex == objectIndex)
			{
				// We're dealing with the right semaphore
#if RTT32_VER >= 528
				if (pCurrentEvent->Event == tDeleteSemaphore)
				{
				// Object is deleted and we haven't found anything
					return 0;
				}
#endif
#if RTT32_VER >= 530
				if (pCurrentEvent->Event == tSemaPulse || pCurrentEvent->Event == tSemaReset)
				{
				// These types always handle event semaphores
					return 1;
				}
#endif
				// Keep an eye out for blocks
				if (pCurrentEvent->Event == tStateBlockedWait || pCurrentEvent->Event == tStateTimedWait)
				{
					foundBlock = 1;
					blockedSourceObjectIndex = prvGetObjectIndex((unsigned int)pCurrentEvent->Source, i);
				}
			}
		}
	}
	return 0;
}

/****************************************************************************
*	prvIsValidName
*	This function is used to determine of a string contains valid characters.
*	Currently only used for spinlocks since they can't be validated any
*	another way.
****************************************************************************/
unsigned int prvIsValidName(const char* str)
{
	unsigned int i;

	for (i = 0; i < TRACE_MAX_NAME_LENGTH && str[i] != 0; i++)
		if (!prvIsValidNameChar(str[i]))
			return 0;

	if (i == TRACE_MAX_NAME_LENGTH)
		return 0;

	return 1;
}

/****************************************************************************
*	prvIsValidChar
*	This function is used to determine of if a character is a valid string
*	character.
****************************************************************************/
unsigned int prvIsValidNameChar(char c)
{
	if (((int)c > 31) && ((int)c < 127))
		return 1;

	return 0;
}

/****************************************************************************
*	prvGetCPUFromEvent
*	This function returns the CPU number if the version supports it
****************************************************************************/
int prvGetCPUFromEvent(struct rtkTraceRec* rec)
{
#if RTK32_VER > 596
	return rec->CPU;
#else
	return 0;
#endif // RTK32_VER > 596
}

/****************************************************************************
*	prvGetTimestampFromEvent
*	If SEQUENTIAL mode is not used, returns the timestamp
****************************************************************************/
unsigned long long prvGetTimestampFromEvent(struct rtkTraceRec* rec)
{
#ifdef TRACE_SEQUENTIAL_TIMESTAMP_MODE
	// We use timestampPadding as counter since that value will be reset before export
	// We bit shift by 2 so we can still modify the timestamp with -3 to +3 and have them end up in the correct order
	return (++timestampPadding) << 2;
#else
	RTKFineTime time;

	if (rec->TimeStamp < previousTimestamp)
	{
		// The new timestamp is lower than the previous, check for timestamping issue
		if (previousTimestamp - rec->TimeStamp > 0x00FFFFFF)
		{
			// The difference is large enough that this most likely is a proper wrapping
			timestampPadding += 0x100000000;
		}
		else
		{
			// The difference is so small that this most likely is an out of order timestamp issue
			FILE_WRITE_TIMESTAMP_WARNING();
		}
	}

	time = timestampPadding + (unsigned long long)rec->TimeStamp;

	previousTimestamp = rec->TimeStamp;

	// Return the requested resolution
	if (RTKConfig.Flags & RF_TRACE_TIME_NS)
		return FTTimeToNanoSecs(&time);
	else if (RTKConfig.Flags & RF_TRACE_TIME_US)
		return FTTimeToMicroSecs(&time);
#endif // TRACE_SEQUENTIAL_TIMESTAMP_MODE
}

/****************************************************************************
*	prvGetTimestampDiff
*	This function returns the difference in nanoseconds between two events.
*	Can be used to detect if enough time has passed for consecutive interrupt
*	events to be split into two different interrupts.
****************************************************************************/
unsigned long long prvGetTimestampDiff(struct rtkTraceRec* high, struct rtkTraceRec* low)
{
	RTKFineTime time;

	if (high->TimeStamp < low->TimeStamp)
	{
		// The later timestamp is lower, check for out of order timestamp issue
		time = 0x100000000 + high->TimeStamp - low->TimeStamp;
		if (low->TimeStamp - high->TimeStamp <= 0x00FFFFFF)
		{
			// The difference is so small that this most likely is an out of order timestamp issue
			FILE_WRITE_TIMESTAMP_WARNING();
		}
	}
	else
		time = high->TimeStamp - low->TimeStamp;

	// Always compares to nano seconds
	return FTTimeToNanoSecs(&time);
}
