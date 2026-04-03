/*
 * command_router.h — voice command keyword router (NORA/ESP32 side)
 *
 * Receives a plain-text transcript from CloudUpload_Transcribe() and routes
 * it to the correct destination based on keyword matching:
 *
 *   Rule A — media/YouTube keywords (play, stop, pause, next, previous, volume)
 *             → HTTP POST {"command":"..."} to PC server at PC_IP:PC_PORT/command
 *
 *   Rule B — display/screen keywords (show, display, screen, clear, update)
 *             → send "CMD:<text>\n" over UART to STM32
 *
 *   Rule C — no keyword matched
 *             → send "CMD:UNKNOWN\n" over UART and log to debug serial
 *
 * CONFIGURATION — edit PC_IP and PC_PORT before building:
 */

#ifndef COMMAND_ROUTER_H
#define COMMAND_ROUTER_H

/* ── PC server address ───────────────────────────────────────────────────── */
#define PC_IP    "10.100.102.7"    /* IP address of the PC running pc_server.py */
#define PC_PORT  5000              /* port pc_server.py listens on               */

/* ── Public API ──────────────────────────────────────────────────────────── */

/*
 * CommandRouter_Init — initialise internal state.
 * Call once after WiFi connects and UART is ready.
 */
void CommandRouter_Init(void);

/*
 * CommandRouter_Route — inspect transcript, apply routing rules, dispatch.
 *
 *   transcript : null-terminated plain-text string from Speech-to-Text
 *                (e.g. "play Beatles", "show temperature", "turn on lights")
 *
 * This function blocks until the HTTP POST or UART write completes.
 * Call from a task (not from an ISR).
 */
void CommandRouter_Route(const char *transcript);

#endif /* COMMAND_ROUTER_H */
