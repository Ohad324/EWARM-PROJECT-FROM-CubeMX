/*********************************************************************
*                    SEGGER RTT — public API header
*
* J-Link locates the control block by scanning RAM for the magic
* string "SEGGER RTT" in the first 16 bytes.  No special linker
* placement is required.
*********************************************************************/
#ifndef SEGGER_RTT_H
#define SEGGER_RTT_H

#include <stdint.h>
#include "SEGGER_RTT_Conf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------
 * Channel mode flags
 *------------------------------------------------------------------*/
#define SEGGER_RTT_MODE_NO_BLOCK_SKIP        (0u) /* drop data if full   */
#define SEGGER_RTT_MODE_NO_BLOCK_TRIM        (1u) /* trim data if full   */
#define SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL   (2u) /* spin until drained  */
#define SEGGER_RTT_MODE_MASK                 (3u)

/*--------------------------------------------------------------------
 * Ring-buffer descriptors (layout must match J-Link expectations)
 *------------------------------------------------------------------*/
typedef struct {
    const char*    sName;        /* human-readable channel name          */
    char*          pBuffer;      /* ring buffer storage                  */
    unsigned       SizeOfBuffer; /* total bytes in pBuffer               */
    unsigned       WrOff;        /* write offset  (written by target)    */
    unsigned       RdOff;        /* read  offset  (written by J-Link)    */
    unsigned       Flags;        /* SEGGER_RTT_MODE_*                    */
} SEGGER_RTT_BUFFER_UP;

typedef struct {
    const char*    sName;
    char*          pBuffer;
    unsigned       SizeOfBuffer;
    unsigned       WrOff;        /* write offset  (written by J-Link)    */
    unsigned       RdOff;        /* read  offset  (written by target)    */
    unsigned       Flags;
} SEGGER_RTT_BUFFER_DOWN;

/*--------------------------------------------------------------------
 * Control block — J-Link finds this by the "SEGGER RTT" magic string
 *------------------------------------------------------------------*/
typedef struct {
    char                   acID[16];   /* "SEGGER RTT\0\0\0\0\0" */
    int                    MaxNumUpBuffers;
    int                    MaxNumDownBuffers;
    SEGGER_RTT_BUFFER_UP   aUp  [SEGGER_RTT_MAX_NUM_UP_BUFFERS];
    SEGGER_RTT_BUFFER_DOWN aDown[SEGGER_RTT_MAX_NUM_DOWN_BUFFERS];
} SEGGER_RTT_CB;

/* Exported so advanced users can inspect the control block directly */
extern SEGGER_RTT_CB _SEGGER_RTT;

/*--------------------------------------------------------------------
 * Public API
 *------------------------------------------------------------------*/

/** One-time initialisation — called automatically on first use. */
void SEGGER_RTT_Init(void);

/**
 * Write NumBytes from pBuffer to up-channel BufferIndex.
 *
 * In BLOCK_IF_FIFO_FULL mode the call spins until all bytes are
 * accepted (complete transfer guaranteed).
 * In NO_BLOCK_SKIP mode it returns the number of bytes actually
 * written (may be less than NumBytes).
 */
unsigned SEGGER_RTT_Write(unsigned BufferIndex,
                           const void* pBuffer,
                           unsigned NumBytes);

/** Write a C string (without the NUL terminator). */
unsigned SEGGER_RTT_WriteString(unsigned BufferIndex, const char* s);

/** Returns number of bytes available in the up-channel ring buffer. */
unsigned SEGGER_RTT_GetAvailWriteSpace(unsigned BufferIndex);

#ifdef __cplusplus
}
#endif

#endif /* SEGGER_RTT_H */
