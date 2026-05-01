/*
 * pc_discovery.h — UDP broadcast discovery of the PC running youtube_player.py
 *
 * NORA boots, joins WiFi, then calls pc_discovery_run() which:
 *   1. Sends "NORA_HELLO" UDP broadcast to 255.255.255.255:5001
 *   2. Waits up to timeout_ms for a "PLAYER:<ip>:<port>" reply
 *   3. Stores the IP/port internally
 *
 * Other modules (command_router, music_task) call pc_get_ip() / pc_get_port()
 * at request time to build the URL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PC_DISCOVERY_PORT     5001
#define PC_DEFAULT_PORT       5000
#define PC_IP_FALLBACK        "10.100.102.9"   /* used if discovery fails */

/* Run discovery; returns true if a PLAYER reply was received. */
bool        pc_discovery_run(uint32_t timeout_ms);

/* Returns the discovered IP (or PC_IP_FALLBACK if discovery never succeeded). */
const char *pc_get_ip(void);

/* Returns the discovered port (or PC_DEFAULT_PORT). */
uint16_t    pc_get_port(void);
