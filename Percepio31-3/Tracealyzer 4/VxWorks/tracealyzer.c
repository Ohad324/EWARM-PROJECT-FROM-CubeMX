/*******************************************************************************
 * Tracealyzer Library for VxWorks, v4.2.9
 * Copyright (c) Percepio AB
 * https://percepio.com
 *
 * tracealyzer.c
 *
 * The core implementation of the Tracealyzer Library for VxWorks, intended to
 * make it easier to record VxWorks traces for Tracealyzer.
 *
 * Typical usage (details below):
 *  
 *  tzConfigStorage(...);  // Optional initialization (or default settings)
 *  tzConfigBuffer(...);   // Optional initialization (or default settings)
 *  tzStart(...);          // Start the recording
 *  tzEvent(...);          // Store "User Event" in the trace.
 *  tzStop(...);           // Stop the recording
 *
 * Kernel events like context-switches and API calls are stored automatically
 * in between tzStart and tzStop. The events and information included depends
 * on the parameter "traceLevel" provided to tzStart.  
 *
 * Additional user-defined events ("User Events") can be saved using tzEvent
 * or wvEvent. The tzEvent function is similar to printf(), while the VxWorks
 * function wvEvent is like a binary write(). tzEvent is easier to use, while
 * wvEvent is more efficient. For Tracealyzer to understand wvEvent data, you
 * need to use "View" -> "Configure User Event Interpretation"
 *
 * Terms of Use
 * This software is copyright Percepio AB, but is free for use together with 
 * Percepio products. You may modify this code as you see fit for your own 
 * internal use, but you may only distribute this code in its original form
 * unless written permission is obtained from Percepio AB.
 * 
 * This software is the intellectual property of Percepio AB and may not be
 * sold or in other ways commercially redistributed without explicit written
 * permission by Percepio AB.
 *
 * Disclaimer
 * The code is being delivered to you AS IS and Percepio AB makes no warranty 
 * as to its use or performance. Percepio AB does not and cannot warrant the 
 * performance or results you may obtain by using the software or documentation. 
 * Percepio AB make no warranties, express or implied, as to noninfringement of 
 * third party rights, merchantability, or fitness for any particular purpose. 
 * In no event will Percepio AB or its partners be liable to you for any 
 * consequential, incidental or special damages, including any lost profits or 
 * lost savings, even if a representative of Percepio AB has been advised of 
 * the possibility of such damages, or for any claim by any third party. 
 * Some jurisdictions do not allow the exclusion or limitation of incidental, 
 * consequential or special damages, or the exclusion of implied warranties or 
 * limitations on how long an implied warranty may last, so the above 
 * limitations may not apply to you.
 *
 * Copyright (c) Percepio AB, 2018. All rights reserved.
 * https://percepio.com
 ******************************************************************************/

#include "tracealyzer.h"

#include <private/wvBufferP.h>
#include <private/wvUploadPathP.h>
#include <private/wvFileUploadPathLibP.h>
#include <private/wvSockUploadPathLibP.h>
#include <sysLib.h>
#include <logLib.h>
#include <fcntl.h>
#include <stdio.h>

STATUS tzCreateSession(void);

/*** Private function prototypes *********************************************/
static STATUS prvTzEvent(int channelID, char* channelName, 
		char* formatStr, va_list vl);

static int prvTzEventFormat(const char* formatStr, va_list vl, 
		char * buffer, int byteOffset);

static int writeString(char * buffer, int i, char* str);

static int writeInt8(char * buffer, int i, uint8_t value);

static int writeInt16(char * buffer, int i, uint16_t value);

static int writeInt32(char * buffer, int i, uint32_t value);

static int writeFloat(char * buffer, int i, float value);

static int writeDouble(char * buffer, int i, double value);

STATUS tzBufferFullHook(void);

WV_LOG		  wvLog;
BUFFER_ID	  wvBufId;		    /* Event buffer ID used */
UPLOAD_ID	  wvUpPathId;	    /* Upload-path id used */
WV_UPLOADTASK_ID  wvUpTaskId;	/* Upload-task id used */

typedef struct{
	int storageMode;
	int storageMethod;
	char* arg1; /* Host address or file path */ 
	int arg2;	/* File options or port */
	int storeOnStop;
} StorageSettingsType;

typedef struct{
	UPLOAD_ID (*Create)(char* arg1, int arg2);
	STATUS (*Close)(UPLOAD_ID upId);
} storageInterfaceType;


storageInterfaceType storageInterface[2] = { {wvFileUploadPathCreate, wvFileUploadPathClose},
										    {wvSockUploadPathCreate, wvSockUploadPathClose} };

static rBuffCreateParamsType rBuffConfig;
static rBuffCreateParamsType* ptrRBuffConfig = NULL;

int SessionCreated = 0;
    
/******************************************************************************
* StorageSettings
*
* Specifies the default setup. The settings correspond to the tzConfigStorage
* parameters and can be overridden using this function.
*
* The default setup is deferred file storage, i.e. keep the latest
* part of the trace in the trace buffer (rBuff) until tzStop is called. 
* Then save the trace buffer to a file on the target system ("tz.wvr").
*
*****************************************************************************/
StorageSettingsType StorageSettings = { TZ_MODE_DEFERRED, TZ_METHOD_FILE, "tz.wvr", O_CREAT | O_TRUNC, 1 };

/******************************************************************************
 * tzStart
 * 
 * Starts the VxWorks recording.
 *
 * Call tzConfigStorage and tzConfigBuffer before to configure the session.
 * Otherwise the default settings will be used (see StorageSettings above).
 * 
 * Parameters:
 * - traceLevel: The level of detail in the recording
 *     TZ_TRACE_LEVEL_CONTEXT_SWITCH: Only context-switches (tasks and ISRs)
 *     TZ_TRACE_LEVEL_TASK_STATE: Adds task state info (ready, waiting, ...)
 *     TZ_TRACE_LEVEL_OBJECT_AND_SYSTEM: All details (recommended).
 *
 *****************************************************************************/
STATUS tzStart(int traceLevel)
{
	if (! SessionCreated)
	{
		tzCreateSession();
	}
	
    if (StorageSettings.storageMode == TZ_MODE_CONTINUOUS)
	{	
		wvUpPathId = storageInterface[StorageSettings.storageMethod].Create(StorageSettings.arg1, StorageSettings.arg2);
		wvUpTaskId = wvUploadStart (wvCurrentLogGet (), wvUpPathId, TRUE);
	}
	
	switch (traceLevel)
	{		
		case TZ_TRACE_LEVEL_OBJECT_AND_SYSTEM:
			traceLevel = WV_CLASS_3;
			break;
		case TZ_TRACE_LEVEL_TASK_STATE:
			traceLevel = WV_CLASS_2;
			break;
		default:
			traceLevel = WV_CLASS_1;
			break;
	}
	
	wvEvtClassSet(traceLevel);
	    
	wvEvtLogStart();	   
	
	return (OK);
}

/******************************************************************************
 * tzStop
 * 
 * Stops the VxWorks recording. Also uploads the trace, when in deferred upload
 * mode and the "storeOnStop" option is used (this is default). 
 *
 *****************************************************************************/
STATUS tzStop(void)
{
	wvEvtLogStop ();
	if ((StorageSettings.storageMode == TZ_MODE_DEFERRED) && (StorageSettings.storeOnStop == 1))
	{
		wvUpPathId = storageInterface[StorageSettings.storageMethod].Create(StorageSettings.arg1, StorageSettings.arg2);
		wvUpTaskId = wvUploadStart (wvCurrentLogGet (), wvUpPathId, FALSE);
	}
	    
	wvUploadStop (wvUpTaskId);
	storageInterface[StorageSettings.storageMethod].Close(wvUpPathId);
	wvLogDelete (wvCurrentLogListGet (), wvCurrentLogGet ());
	return OK;
}

/******************************************************************************
* tzEvent
*
* Creates a Tracealyzer "User Event", supporting formatted text and data.
* This can be used instead of a "printf" to get custom log messages into the
* trace, where also the context is visualized (task-switches etc.)
*
* Storing a User Event is much faster than a printf because the formatting is
* done in the Tracealyzer host application, not in the target system.
*
* This builds on VxWorks user-defined events, i.e. wvEvent() in wvLib.h, but
* also provides a structure to the data that allows Tracealyzer to interpret
* it. However, this adds a 4 byte marker to each event that allows Tracealyzer
* to recognize events with this encoding.
*
* Parameters:
*
*  - char* channelName: a name for a Tracealyzer User Event channel where this
*    event should be included. By using null or an empty string here, no
*    channelName is included which makes the event smaller. In that case, the
*    user event will be assigned to the Default Channel in Tracealyzer.
*    The maximum length is 128 characters, assuming MAX_ARG_SIZE is respected.
*
*  - char* formatStr: the format string (label) displayed for the event in
*    Tracealyzer, including format specifiers for data arguments similar to
*    a classic printf call (see supported format specifiers below).
*    The maximum length is 1024 characters, assuming MAX_ARG_SIZE is adjusted
*    to allow events longer than 256 bytes (see below).
*
*  - optional data arguments, where their types are specified in the format
*    string parameter.
*
* Returns:
*  - OK (0) if event stored successfully.
*  - ERROR (-1) if the event could not be stored.
*
* Format specifiers supported:
*  %d  - 32 bit signed integer
*  %u  - 32 bit unsigned integer
*  %f  - 32 bit float
*  %s  - string (max 1024 chars)
*  %hd - 16 bit signed integer
*  %hu - 16 bit unsigned integer
*  %bd - 8 bit signed integer
*  %bu - 8 bit unsigned integer
*  %lf - double-precision (64 bit) float
*
* Examples:
*
* - tzEvent("Debug", "Entering function foo()");
*
* - tzEvent("Voltage U1", "U1: %lf v", voltage_u1);
*
* - tzEvent("FSM State Changes", "(%s) %s -> %s (%s)", "FSM_1",
*          fsm1StateNames[oldStateID], fsm1StateNames[newStateID],
* 			fsm1EventNames[eventID]);
*
* NOTE 1:
* This function uses a local buffer where all data is packed, including
* string characters. The size of this buffer is defined by MAX_ARG_SIZE in
* tracealyzer.h, which default value is 256. This is used to store the
* following data about the event:
*  - 4 "marker" bytes that identifies the wvEvent as a tzEvent,
*  - the channelName string, including zero-termination,
*  - the format string, including zero-termination, and
*  - all data arguments in native, binary format, where string arguments are
*    stored character-by-character, i.e., not a as a pointer.
*
* If this buffer size is exceeded by the arguments, the event is not stored
* and ERROR (-1) is returned. In that case, increase MAX_ARG_SIZE or shorten
* the event data (e.g., format string or channel name). In case you only
* store shorter events, you can reduce MAX_ARG_SIZE to reduce stack usage.
*
* NOTE 2:
* The user events are stored using the wvEvent function (see wvLib.h).
* that requires a numeric ID for the event ("usrEventId"). This is not used
* by Tracealyzer, so the constant WVEVENT_ID (= 0) is used for this purpose.
* This is defined in tracealyzer.h and can be modified if desired.
*
* NOTE 3:
* Another way of storing user events is to call wvEvent directly. This is even
* faster that tzEvent and does not require the marker bytes used by tzEvent.
* To make Tracealyzer understand such arbitrary wvEvent data, you need to
* provide the interpretation from raw bytes into values. This is done using
* "View" -> "Configure User Event Interpretation" in the main menu.
*****************************************************************************/
STATUS tzEvent(char* channelName, char* formatStr, ...)
{
	va_list vl;
	STATUS res;

	va_start(vl, formatStr);
	res = prvTzEvent(WVEVENT_ID, channelName, formatStr, vl);
	va_end(vl);

	return res;
}

/******************************************************************************
* tzConfigStorage
*
* Configures the VxWorks recording.
*
* The first step in configuring the recording. May optionally be
* followed by tzConfigBuffer(), thereafter you may call tzStart().
*
* if tzConfigStorage is not called, the default settings will be used (see
* StorageSettings in the .c file).
*
* Parameters:
*
* - storageMode:
*     TZ_MODE_CONTINUOUS (0): Trace is continuously stored to a file or socket.
*     TZ_MODE_DEFERRED (1): Trace is only stored on command (tzStop).
*
* - storageMethod:
*     TZ_METHOD_FILE (0): Trace is stored on the target file system
*     TZ_METHOD_SOCKET (1): Trace is uploaded to host via a TCP socket
*
* - arg1: (if using TZ_METHOD_SOCKET)
*         The address of the computer running Tracealyzer (hostname or IP)
*
*         (if using TZ_METHOD_FILE)
*         The path of the trace file.
*
* - arg2: (if using TZ_METHOD_SOCKET)
*         The remote TCP port.
*
*         (if using TZ_METHOD_FILE)
*         File open attributes/flags. Should be O_CREAT | O_TRUNC.
*
* - storeOnStop: If set to 1, calling tzStop will also store the trace,
*                assuming the storage mode is TZ_MODE_DEFERRED.
*
* Examples:
*
* tzConfigStorage(TZ_MODE_DEFERRED, TZ_METHOD_FILE, "tz.wvr", O_CREAT | O_TRUNC, 1);
*
*   The recording is kept in the memory buffer (rBuff) until tzStop is called,
*   then saved to file ("tz.wvr" on the device file system). The oldest events
*   are overwritten when the buffer becomes full.
*   This limits the length of the recording to the size of the memory buffer
*   (see tzConfigBuffer, by default 256 KB), but the recording can remain
*   active continuously, like a "flight recorder".
*   To view the trace in Tracealyzer, you need to upload the trace file
*   using FTP or similar.
*
* tzConfigStorage(TZ_MODE_CONTINUOUS, TZ_METHOD_FILE, "tz.wvr", O_WRONLY, 0);
*
*   The recording is written continuously to file, i.e. "tz.wvr", in the target
*   system. Allows for traces of unlimited duration, as long as there is
*   storage space in the target system.
*   To view the trace in Tracealyzer, you need to upload the trace file
*   using FTP or similar.
*
* tzConfigStorage(TZ_MODE_CONTINUOUS,TZ_METHOD_SOCKET,"192.168.10.91",19379,0);
*
*   The recording is written continuously to TCP socket, i.e. streaming mode to
*   host. Allows for traces of unlimited duration, as long as there is storage
*   space in the host system.
*
*****************************************************************************/
STATUS tzConfigStorage(int storageMode, int storageMethod, char* arg1, int arg2, int storeOnStop)
{
	StorageSettings.storageMode = storageMode;
	StorageSettings.storageMethod = storageMethod;
	StorageSettings.arg1 = arg1;
	StorageSettings.arg2 = arg2;
	StorageSettings.storeOnStop = storeOnStop;

	return (OK);
}

/******************************************************************************
 * tzConfigureBuffer
 * 
 * Configures the memory buffer (rBuff) used for temporary storage of the trace
 * data, before it is stored to disk or transferred to host using a TCP socket.
 *
 * This is an (optional) second step of initializing a recording session, after
 * tzConfigureStorage. If this is not called explicitly before calling tzStart,
 * a default buffer size is configured by tzConfigDefault.
 *
 * Parameters:
 *
 *  - bufferCount: The (maximum) number of sub-buffers used. Valid range: 2-16.
 *
 *  - bufferSize: The size (in bytes) of each sub-buffer.
 *
 * When using deferred storage mode, this buffer size determined the length of
 * the recording. When using continous storage mode, it is important to have
 * a sufficiently large buffer to avoid intermittent data loss in the trace
 * streaming.
 *  
 *****************************************************************************/
STATUS tzConfigureBuffer(int bufferCount, int bufferSize)
{	
	rBuffConfig.errorHandler = tzBufferFullHook;
	rBuffConfig.buffSize = (size_t)bufferSize;
	rBuffConfig.minimum = 2;
	
	if (bufferCount < 2)
		bufferCount = 2;
	
	if (bufferCount > 16)
		bufferCount = 16;
	
	rBuffConfig.maximum = bufferCount;
	rBuffConfig.options = 0;
	rBuffConfig.threshold = (int)(rBuffConfig.buffSize / 2);
	
	wvPartitionSet (memSysPartId);
	rBuffConfig.sourcePartition = wvPartitionGet ();	
	
	ptrRBuffConfig = &rBuffConfig;
	
	return (OK);
}

/******************************************************************************
* tzConfigDefault
*
* Configures a default trace buffer with a total size of 256 KB. 
* Is called during tzStart, if tzConfigureBuffer has not been called before.
*
******************************************************************************/
STATUS tzConfigDefault()
{
	/* Sets up four buffers á 64 KB = 256 KB */
	tzConfigureBuffer(4, 0x10000);
	
	return (OK);
}

/******************************************************************************
* tzBufferFullHook
*
* This function is called by VxWorks when the event buffer becomes full.
*
* You may your own code here, e.g. to stop the recording to avoid overwriting
* the older data. But note that this function is a callback, that may execute
* within kernel context, so be careful what you add here.
*
******************************************************************************/
STATUS tzBufferFullHook(void)
{
	return OK;
}

void tzConfigRBuffThreshold(int threshold)
{
	ptrRBuffConfig->threshold = threshold;
}

void tzConfigRBuffOptions(int options)
{
	ptrRBuffConfig->options = options; /* rBuffCreate parameter "options" */
}

void tzConfigRBuffPartition(PART_ID pid)
{
	ptrRBuffConfig->sourcePartition = pid;		
}

rBuffCreateParamsType* tzConfigGet(void)
{
	return ptrRBuffConfig;
}

void tzConfigSet(rBuffCreateParamsType* cfg)
{
	ptrRBuffConfig = cfg; 
}

STATUS tzCreateSession(void)
{
	if (wvCurrentLogListGet () == NULL)
	{
		wvLogListCreate ();
	}

	if (ptrRBuffConfig == NULL)
	{
		tzConfigDefault();
	}
	
	if ((wvBufId = rBuffCreate (ptrRBuffConfig)) == NULL)
	{
		logMsg ("tzCreateSession: Error creating buffer.\n",0,0,0,0,0,0);
	    return (ERROR);
	}
	
	if (wvLogCreate (wvBufId) == NULL)
	{
		logMsg ("tzCreateSession: Error creating log\n", 0, 0, 0, 0, 0, 0);
	    rBuffDestroy (wvBufId);
	    return (ERROR);
	}
	
	SessionCreated = 1;
	
	return (OK);
}

STATUS tzConfigStorageArgs(char* arg1, int arg2)
{
	StorageSettings.arg1 = arg1;
	StorageSettings.arg2 = arg2;
	return (OK);
}

STATUS tzConfigStorageMode(int storageMode)
{
	StorageSettings.storageMode = storageMode;
	return (OK);
}

STATUS tzConfigStorageMethod(int storageMethod)
{
	StorageSettings.storageMethod = storageMethod;
	return (OK);
}

STATUS tzConfigStoreOnStop(int shouldStoreOnStop)
{
	StorageSettings.storeOnStop = shouldStoreOnStop;
	return (OK);
}


/* Private helper function */
static STATUS prvTzEvent(int channelID, char* channelName, 
		char* formatStr, va_list vl)
{
	int pos;
	int minlen;
	STATUS res;
	char tempDataBuffer[MAX_ARG_SIZE];
	char emptyStr = 0;
	
	if (channelName == NULL)
	{
		channelName = &emptyStr;
	}
	
	if (formatStr == NULL)
	{
		formatStr = &emptyStr;
	}
		
	minlen = strlen(channelName) + 1 + strlen(formatStr) + 1 + 4;

	if (minlen > MAX_ARG_SIZE)
	{
		return ERROR;
	}

	tempDataBuffer[0] = 0xA;
	tempDataBuffer[1] = 0xC;
	tempDataBuffer[2] = 0xD;
	tempDataBuffer[3] = 0xC;

	pos = writeString(tempDataBuffer, 4, channelName);
	
	if (pos < 0) return ERROR;
	
	pos = writeString(tempDataBuffer, pos, formatStr);
	
	if (pos < 0) return ERROR;
	
	pos = prvTzEventFormat(formatStr, vl, tempDataBuffer, pos);

	if (pos < 0) return ERROR;
	
	res = wvEvent(channelID, tempDataBuffer, pos);

	return res;
}

/* Private helper function */
static int prvTzEventFormat(const char* formatStr, va_list vl, 
		char * buffer, int byteOffset)
{
	int formatStrIndex = 0;
	int i = byteOffset;

	while (formatStr[formatStrIndex] != '\0')
	{
		if (formatStr[formatStrIndex] == '%')
		{
			formatStrIndex++;

			while ((formatStr[formatStrIndex] >= '0'
			        && formatStr[formatStrIndex] <= '9')
			        || formatStr[formatStrIndex] == '#'
			        || formatStr[formatStrIndex] == '.')
			{
				formatStrIndex++;
			}

			if (formatStr[formatStrIndex] != '\0')
			{
				switch (formatStr[formatStrIndex])
				{
					case 'd':
						i = writeInt32(buffer, i,
						        (uint32_t) va_arg(vl, uint32_t));
						break;
					case 'x':
					case 'X':
					case 'u':
						i = writeInt32(buffer, i,
						        (uint32_t) va_arg(vl, uint32_t));
						break;

					case 's':
						/* Eclipse code analysis may incorrectly show
  	  	  	  	  	  	   Syntax Error on the va_arg expression. */
												
						i = writeString(buffer, i, (char*)va_arg(vl, char*));
						break;

						
					case 'f':
						
						/* Eclipse code analysis may incorrectly show
						   Syntax Error on the va_arg expression. */
						
						i = writeFloat(buffer, i, (float) va_arg(vl, double));
						
						/* Yes, "double" is correct here (float is promoted)*/
						
						break;

					case 'l':
						formatStrIndex++;
						switch (formatStr[formatStrIndex])
						{
							case 'f':
								
								/* Eclipse code analysis may incorrectly show
								   Syntax Error on the va_arg expression. */
								
								i = writeDouble(buffer, i,
								        (double) va_arg(vl, double)); 
								break;
						}
						break;

					case 'h':
						formatStrIndex++;
						switch (formatStr[formatStrIndex])
						{
							case 'd':
								i = writeInt16(buffer, i,
								        (uint16_t) va_arg(vl, uint32_t));
								break;

							case 'u':
								i = writeInt16(buffer, i,
								        (uint16_t) va_arg(vl, uint32_t));
								break;
						}
						break;

					case 'b':
						formatStrIndex++;
						switch (formatStr[formatStrIndex])
						{

							case 'd':
								i = writeInt8(buffer, i,
								        (uint8_t) va_arg(vl, uint32_t));
								break;

							case 'u':
								i = writeInt8(buffer, i,
								        (uint8_t) va_arg(vl, uint32_t));
								break;
						}
						break;
				}
			}
			else
				break;
		}
		formatStrIndex++;
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

/* Private helper function */
static int writeString(char * buffer, int i, char* str)
{
	int j = 0;
	do
	{
		if (i >= MAX_ARG_SIZE)
		{
			return -1;
		}

		buffer[i++] = str[j++];

	}
	while (buffer[i - 1] != 0);

	return i;
}

/* Private helper function */
static int writeInt8(char * buffer, int i, uint8_t value)
{
	if (i + 1 >= MAX_ARG_SIZE)
	{
		return -1;
	}

	buffer[i] = value;

	return i + 1;
}

/* Private helper function */
static int writeInt16(char * buffer, int i, uint16_t value)
{
	while ((i % 2) != 0)
	{
		if (i >= MAX_ARG_SIZE)
		{
			return -1;
		}

		buffer[i] = 0;
		i++;
	}

	if (i + 2 > MAX_ARG_SIZE)
	{
		return 255;
	}

	((uint16_t*) buffer)[i / 2] = value;

	return i + 2;
}

/* Private helper function */
static int writeInt32(char * buffer, int i, uint32_t value)
{
	while ((i % 4) != 0)
	{
		if (i >= MAX_ARG_SIZE)
		{
			return -1;
		}

		buffer[i] = 0;
		i++;
	}

	if (i + 4 > MAX_ARG_SIZE)
	{
		return -1;
	}

	((uint32_t*) buffer)[i / 4] = value;

	return i + 4;
}

/* Private helper function */
static int writeFloat(char * buffer, int i, float value)
{
	while ((i % 4) != 0)
	{
		if (i >= MAX_ARG_SIZE)
		{
			return -1;
		}

		buffer[i] = 0;
		i++;
	}

	if (i + 4 > MAX_ARG_SIZE)
	{
		return -1;
	}

	((float*) buffer)[i / 4] = value;

	return i + 4;
}

/* Private helper function */
static int writeDouble(char * buffer, int i, double value)
{
	uint32_t * dest;
	uint32_t * src = (void*) &value;

	while (i % 4 != 0)
	{
		if (i >= MAX_ARG_SIZE)
		{
			return -1;
		}

		buffer[i] = 0;
		i++;
	}

	if (i + 8 > MAX_ARG_SIZE)
	{
		return -1;
	}

	dest = (uint32_t*) &buffer[i];

	dest[0] = src[0];
	dest[1] = src[1];

	return i + 8;
}
