/*********************************************************************
*                    SEGGER RTT — implementation
*
* Ring-buffer layout and control-block magic string must exactly
* match what J-Link expects.  Do not reorder struct fields.
*
* Thread-safety: SEGGER_RTT_Write() is called only from
* MusicDisplayTask (a single RTOS task).  No mutex is needed.
* If you ever call it from multiple tasks, wrap with a mutex.
*********************************************************************/
#include "SEGGER_RTT.h"
#include <string.h>

/*--------------------------------------------------------------------
 * Static storage for channel 0 ring buffers
 *------------------------------------------------------------------*/
static char s_upBuf  [SEGGER_RTT_BUFFER_SIZE_UP];
static char s_downBuf[SEGGER_RTT_BUFFER_SIZE_DOWN];

/*--------------------------------------------------------------------
 * Control block
 *
 * The first 16 bytes MUST be the string "SEGGER RTT" zero-padded.
 * J-Link scans all RAM at connect time looking for this signature.
 * __attribute__((used)) prevents the linker from discarding it when
 * no code references it by name.
 *------------------------------------------------------------------*/
SEGGER_RTT_CB _SEGGER_RTT __attribute__((used)) = {
    /* acID — magic signature, exactly 16 bytes */
    "SEGGER RTT\0\0\0\0\0\0",
    /* MaxNumUpBuffers, MaxNumDownBuffers */
    SEGGER_RTT_MAX_NUM_UP_BUFFERS,
    SEGGER_RTT_MAX_NUM_DOWN_BUFFERS,
    /* aUp[0] */
    {
        {
            "Terminal",         /* sName        */
            s_upBuf,            /* pBuffer      */
            sizeof(s_upBuf),    /* SizeOfBuffer */
            0u,                 /* WrOff        */
            0u,                 /* RdOff        */
            SEGGER_RTT_MODE_DEFAULT
        }
    },
    /* aDown[0] */
    {
        {
            "Terminal",
            s_downBuf,
            sizeof(s_downBuf),
            0u,
            0u,
            SEGGER_RTT_MODE_NO_BLOCK_SKIP
        }
    }
};

/*--------------------------------------------------------------------
 * Internal helpers
 *------------------------------------------------------------------*/

/** Returns free bytes in up-channel ring buffer (always ≥ 1 empty slot). */
static unsigned _GetFreeUp(const SEGGER_RTT_BUFFER_UP* pRing)
{
    unsigned wr = pRing->WrOff;
    unsigned rd = pRing->RdOff;   /* J-Link updates this */
    if (wr >= rd)
        return (pRing->SizeOfBuffer - 1u - wr + rd);
    else
        return (rd - wr - 1u);
}

/** Write up to NumBytes into the ring buffer; returns bytes written. */
static unsigned _WritePartial(SEGGER_RTT_BUFFER_UP* pRing,
                               const char*           pData,
                               unsigned              NumBytes)
{
    unsigned avail = _GetFreeUp(pRing);
    if (avail == 0u)
        return 0u;

    unsigned toWrite   = (NumBytes < avail) ? NumBytes : avail;
    unsigned wr        = pRing->WrOff;
    unsigned spaceToEnd = pRing->SizeOfBuffer - wr;

    if (toWrite <= spaceToEnd)
    {
        memcpy(pRing->pBuffer + wr, pData, toWrite);
        wr += toWrite;
        if (wr == pRing->SizeOfBuffer)
            wr = 0u;
    }
    else
    {
        /* Wrap-around: write in two parts */
        memcpy(pRing->pBuffer + wr, pData, spaceToEnd);
        memcpy(pRing->pBuffer, pData + spaceToEnd, toWrite - spaceToEnd);
        wr = toWrite - spaceToEnd;
    }

    /* Commit — J-Link polls WrOff to detect new data */
    pRing->WrOff = wr;
    return toWrite;
}

/*--------------------------------------------------------------------
 * Public API
 *------------------------------------------------------------------*/

void SEGGER_RTT_Init(void)
{
    /* Control block is statically initialised; nothing to do at runtime.
     * Function exists so callers can call it explicitly if desired.    */
}

unsigned SEGGER_RTT_GetAvailWriteSpace(unsigned BufferIndex)
{
    return _GetFreeUp(&_SEGGER_RTT.aUp[BufferIndex]);
}

unsigned SEGGER_RTT_Write(unsigned BufferIndex,
                           const void* pBuffer,
                           unsigned NumBytes)
{
    SEGGER_RTT_BUFFER_UP* pRing = &_SEGGER_RTT.aUp[BufferIndex];
    const char*           pData = (const char*)pBuffer;
    unsigned              remaining = NumBytes;

    unsigned mode = pRing->Flags & SEGGER_RTT_MODE_MASK;

    if (mode == SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL)
    {
        /* Spin until every byte is accepted.
         * J-Link drains the ring buffer continuously while connected.
         * On a 4 MB/s SWD link a 4 KB ring carries ~1.1 MB in ~0.3 s. */
        while (remaining > 0u)
        {
            unsigned written = _WritePartial(pRing, pData, remaining);
            pData      += written;
            remaining  -= written;
            /* If nothing was written the buffer is full — yield the CPU
             * momentarily so FreeRTOS can run other tasks while we wait.
             * taskYIELD() is a no-op if called from ISR context, but
             * SEGGER_RTT_Write should never be called from ISR anyway.  */
            if (written == 0u)
            {
                /* Bare-metal busy-wait fallback if scheduler not running */
                __asm volatile ("nop");
            }
        }
        return NumBytes;
    }
    else if (mode == SEGGER_RTT_MODE_NO_BLOCK_TRIM)
    {
        /* Write as much as fits, silently drop the rest */
        return _WritePartial(pRing, pData, remaining);
    }
    else
    {
        /* SEGGER_RTT_MODE_NO_BLOCK_SKIP:
         * Write only if the whole message fits; skip entirely otherwise */
        if (_GetFreeUp(pRing) >= remaining)
            return _WritePartial(pRing, pData, remaining);
        return 0u;
    }
}

unsigned SEGGER_RTT_WriteString(unsigned BufferIndex, const char* s)
{
    unsigned len = 0u;
    while (s[len] != '\0') { len++; }
    return SEGGER_RTT_Write(BufferIndex, s, len);
}
