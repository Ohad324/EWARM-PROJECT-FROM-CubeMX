/*
 * command_handler.c — STM32-side voice command receiver and dispatcher
 *
 * HOW IT WORKS:
 *   1. UARTReceiveTask detects a "CMD:" prefix in an incoming UART8 message.
 *   2. It calls CommandHandler_Post(msg) which copies the payload into a queue.
 *   3. CommandHandler_TaskEntry wakes up, strips the "CMD:" prefix, and
 *      matches the text against known command keywords.
 *   4. Matched commands invoke the appropriate screen-update stub.
 *      Unknown commands log "CMD:UNKNOWN" over UART.
 *
 * ADDING NEW COMMANDS:
 *   Add a new entry to s_cmdTable[] with a keyword and handler function.
 *   The keyword is matched as a case-insensitive substring of the payload.
 *
 * EXTENDING THE SCREEN API:
 *   Replace the stub bodies in the CmdHandle_* functions with real
 *   TouchGFX / xBleQueue calls once the UI presenters are ready.
 */

#include "command_handler.h"
#include "main.h"           /* huart8, Error_Handler           */
#include <stdbool.h>        /* bool, true, false               */
#include "stm32h7xx_hal.h"  /* HAL_UART_Transmit               */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include <string.h>         /* strncmp, strlen, strncpy, tolower */
#include <stdio.h>          /* snprintf                          */
#include "SEGGER_RTT.h"
#include <ctype.h>          /* tolower                           */

/* ── Internal types ──────────────────────────────────────────────────────── */

typedef struct
{
    const char *keyword;                    /* matched as substring (case-insensitive) */
    void (*handler)(const char *payload);   /* called with the full CMD payload        */
} CmdEntry_t;

/* ── Queue ───────────────────────────────────────────────────────────────── */

static QueueHandle_t s_cmdQueue;

/* CMD_QUEUE_DEPTH: 4 slots — enough to absorb bursts without blocking UART task */
#define CMD_QUEUE_DEPTH  4u

/* Each slot holds a null-terminated CMD payload up to CMD_MAX_LEN bytes. */
typedef char CmdSlot_t[CMD_MAX_LEN + 1u];

/* ── Debug UART helper ───────────────────────────────────────────────────── */

extern UART_HandleTypeDef huart8;

static void SendStr(const char *s)
{
    HAL_UART_Transmit(&huart8, (uint8_t *)s, (uint16_t)strlen(s), 100);
}

/* ── Command handler stubs ───────────────────────────────────────────────── */
/* Replace stub bodies with real TouchGFX signal/xBleQueue calls as the UI   */
/* presenters are developed.                                                   */

static void CmdHandle_Show(const char *payload)
{
    /* "show temperature", "show humidity", "show time" etc.
     * TODO: post MSG_TRACK or a custom MSG_DISPLAY to xBleQueue so
     * the Model::tick() can update the screen. */
    char msg[72];
    snprintf(msg, sizeof(msg), "CMD_ACK:show:%s\n", payload);
    SendStr(msg);
}

static void CmdHandle_Display(const char *payload)
{
    /* "display stats", "display graph" etc. */
    char msg[72];
    snprintf(msg, sizeof(msg), "CMD_ACK:display:%s\n", payload);
    SendStr(msg);
}

static void CmdHandle_Clear(const char *payload)
{
    (void)payload;
    /* Clear the active screen / reset displayed values */
    SendStr("CMD_ACK:clear\n");
}

static void CmdHandle_Update(const char *payload)
{
    /* "update display", "update values" */
    char msg[72];
    snprintf(msg, sizeof(msg), "CMD_ACK:update:%s\n", payload);
    SendStr(msg);
}

static void CmdHandle_Screen(const char *payload)
{
    /* "screen off", "screen brightness" etc. */
    char msg[72];
    snprintf(msg, sizeof(msg), "CMD_ACK:screen:%s\n", payload);
    SendStr(msg);
}

static void CmdHandle_Unknown(const char *payload)
{
    char msg[80];
    snprintf(msg, sizeof(msg), "CMD:UNHANDLED:%s\n", payload);
    SendStr(msg);
}

/* ── Command dispatch table ──────────────────────────────────────────────── */

static const CmdEntry_t s_cmdTable[] =
{
    { "show",    CmdHandle_Show    },
    { "display", CmdHandle_Display },
    { "clear",   CmdHandle_Clear   },
    { "update",  CmdHandle_Update  },
    { "screen",  CmdHandle_Screen  },
};

#define CMD_TABLE_LEN  (sizeof(s_cmdTable) / sizeof(s_cmdTable[0]))

/* ── Case-insensitive substring search ─────────────────────────────────────
 * Returns non-zero if needle is found anywhere in haystack (case-insensitive).
 * ─────────────────────────────────────────────────────────────────────────── */
static int ContainsKeyword(const char *haystack, const char *needle)
{
    size_t hLen = strlen(haystack);
    size_t nLen = strlen(needle);
    if (nLen > hLen) return 0;

    for (size_t i = 0; i <= hLen - nLen; i++)
    {
        size_t j;
        for (j = 0; j < nLen; j++)
        {
            if (tolower((unsigned char)haystack[i + j]) !=
                tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nLen) return 1;
    }
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* ── CommandHandler_Init ─────────────────────────────────────────────────── */
void CommandHandler_Init(void)
{
    s_cmdQueue = xQueueCreate(CMD_QUEUE_DEPTH, sizeof(CmdSlot_t));
    configASSERT(s_cmdQueue);
}

/* ── CommandHandler_Post ─────────────────────────────────────────────────── */
int CommandHandler_Post(const char *msg)
{
    if (!msg) return 0;

    /* Strip "CMD:" prefix if present */
    const char *payload = msg;
    if (strncmp(msg, "CMD:", 4) == 0) payload = msg + 4;

    SEGGER_RTT_WriteString(0, "[CMD-POST] msg=[");
    SEGGER_RTT_WriteString(0, msg);
    SEGGER_RTT_WriteString(0, "]\n");

    CmdSlot_t slot;
    strncpy(slot, payload, CMD_MAX_LEN);
    slot[CMD_MAX_LEN] = '\0';

    /* Non-blocking send: if the queue is full, drop the command rather than
     * blocking UARTReceiveTask (which runs at above-normal priority). */
    return (xQueueSend(s_cmdQueue, slot, 0) == pdTRUE) ? 1 : 0;
}

/* ── CommandHandler_TaskEntry ────────────────────────────────────────────────
 * Blocks on s_cmdQueue, dispatches each command to the matching handler.
 * ─────────────────────────────────────────────────────────────────────────── */
void CommandHandler_TaskEntry(void *arg)
{
    (void)arg;

    CmdSlot_t payload;

    for (;;)
    {
        /* Block indefinitely until a command arrives */
        xQueueReceive(s_cmdQueue, payload, portMAX_DELAY);

        SEGGER_RTT_WriteString(0, "[CMD-DISPATCH] payload=[");
        SEGGER_RTT_WriteString(0, payload);
        SEGGER_RTT_WriteString(0, "]\n");

        /* "UNKNOWN" is a special token sent by NORA when routing fails */
        if (strncmp(payload, "UNKNOWN", 7) == 0)
        {
            SendStr("CMD:UNKNOWN_RECEIVED\n");
            continue;
        }

        /* Scan the dispatch table for a matching keyword */
        bool handled = false;
        for (size_t i = 0; i < CMD_TABLE_LEN; i++)
        {
            if (ContainsKeyword(payload, s_cmdTable[i].keyword))
            {
                s_cmdTable[i].handler(payload);
                handled = true;
                break;
            }
        }

        if (!handled)
        {
            CmdHandle_Unknown(payload);
        }
    }
}
