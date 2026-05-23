/*
 * ntfy_client.h — NORA → PC music-search bridge via ntfy.sh public broker
 *
 * Bypasses iPhone hotspot client-isolation (BUG-004) by routing the
 * NORA → PC search request through the public internet via ntfy.sh.
 * Both NORA and the PC bridge make only OUTGOING HTTPS calls, which the
 * iPhone permits — they never try to talk to each other directly.
 *
 * Flow:
 *   1. NORA POSTs   {"id":N,"query":"..."}   to  /<NTFY_TOPIC_REQ>
 *   2. PC bridge.py subscribes to req topic, forwards to its existing
 *      localhost search service, then POSTs back:
 *      {"id":N,"videoId":"..."}                 to  /<NTFY_TOPIC_RES>
 *   3. NORA polls /<NTFY_TOPIC_RES>?since=<ts>&poll=1 until id matches.
 *
 * Topics are unguessable random strings; collisions on ntfy.sh public
 * service are virtually impossible.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Synchronous music search via ntfy.sh relay.
 *
 *   query        : null-terminated search string (e.g. "Led Zeppelin")
 *   out_video_id : caller-provided buffer for the resolved YouTube videoId
 *   max_len      : size of out_video_id (must be >= 12)
 *   timeout_ms   : total budget for publish + poll
 *
 * Returns true if a non-empty videoId was retrieved.
 */
bool ntfy_search(const char *query,
                 char       *out_video_id,
                 size_t      max_len,
                 uint32_t    timeout_ms);

#ifdef __cplusplus
}
#endif
