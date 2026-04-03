/******************************************************************************
 * Tracealyzer Library for VxWorks, v4.2.9
 * Copyright (c) Percepio AB
 * https://percepio.com
 *
 * tracealyzer.h
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
 * need to use "View" -> "Configure User Event Interpretation".
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
 *****************************************************************************/


#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <wvLib.h>

/* Level of Detail in the recording */
#define TZ_TRACE_LEVEL_CONTEXT_SWITCH 0
#define TZ_TRACE_LEVEL_TASK_STATE 1
#define TZ_TRACE_LEVEL_OBJECT_AND_SYSTEM 2

/* Storage Mode */
#define TZ_MODE_CONTINUOUS 0
#define TZ_MODE_DEFERRED 1

/* Storage Method */
#define TZ_METHOD_FILE 0
#define TZ_METHOD_SOCKET 1

/******************************************************************************
* tzStart
*
* Starts the VxWorks recording.
*
* Call tzConfigStorage and tzConfigBuffer before to configure the session.
* Otherwise the default settings will be used (StorageSettings in the .c file).
*
* Parameters:
* - traceLevel: The level of detail in the recording
*     TZ_TRACE_LEVEL_CONTEXT_SWITCH: Only context-switches (tasks and ISRs)
*     TZ_TRACE_LEVEL_TASK_STATE: Adds task state info (ready, waiting, ...)
*     TZ_TRACE_LEVEL_OBJECT_AND_SYSTEM: All details (recommended).
*
*****************************************************************************/
STATUS tzStart(int traceLevel);

/******************************************************************************
* tzStop
*
* Stops the VxWorks recording. Also uploads the trace, when in deferred upload
* mode and the "storeOnStop" option is used (this is default). 
*
*****************************************************************************/
STATUS tzStop(void);

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
*  - 4 byte "marker" that identifies the wvEvent as a tzEvent,
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
STATUS tzEvent(char* channelName, char* formatStr, ...);

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
STATUS tzConfigStorage(int storageMode, 
					   int storageMethod, 
					   char* arg1, 
					   int arg2, 
					   int storeOnStop);

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
STATUS tzConfigureBuffer(int bufferCount, int bufferSize);

/******************************************************************************
 * MAX_ARG_SIZE
 * Integer constant (macro) that defines the size of the local buffer used in 
 * tracealyzerUserEvent, where all data is packed, including
 * string characters. Default value is 256. Increase this to allow longer 
 * events, or decrease to reduce stack usage.
 *****************************************************************************/ 
#define MAX_ARG_SIZE 256

/******************************************************************************
 * WVEVENT_ID
 * Integer constant (macro) that defines the event ID used in the calls to 
 * wvEvent (wvLib) that stores the user event data. This is not used by
 * Tracealyzer but is used by System Viewer. If you like to put Tracealyzer
 * user events on another (System Viewer) channel ID, this can be modified 
 * without effect on the Tracealyzer functionality.
 *****************************************************************************/ 
#define WVEVENT_ID 0
