/*
 * command_router.c — voice command keyword router (NORA/ESP32 side)
 *
 * Receives a transcript string from CloudUpload_Transcribe() and applies
 * the routing rules in order:
 *
 *   Rule A1 — "play": music_request() → YouTube search via music_task (voice path)
 *   Rule A2 — stop, pause, next, previous, volume
 *              → HTTP POST {"command":"<transcript>"} to PC server
 *
 *   Rule B — DISPLAY keywords: show, display, screen, clear, update
 *             → UART "CMD:<transcript>\n" to STM32
 *
 *   Rule D — Bare song / artist name (no "play" prefix), ≥3 non-space chars
 *             → music_request(<whole transcript>) — treated as a YouTube search
 *             e.g. "doors", "Zohar Argov", "Beatles"
 *
 *   Rule C — No match (transcript too short / garbage)
 *             → UART "CMD:UNKNOWN\n" + ESP_LOG debug print
 *
 * ASSUMPTIONS:
 *   - WiFi is connected when CommandRouter_Route() is called.
 *   - The PC server is running and reachable at PC_IP:PC_PORT.
 *   - UART_NUM_1 is already initialised by nora_ble_bridge.c (TX=17, RX=18).
 */

#include "command_router.h"
#include "cloud_upload.h"
#include "music_task.h"
#include "pc_discovery.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>  /* strncasecmp */

static const char *TAG = "cmd_router";

/* UART port shared with the rest of the bridge */
#define ROUTER_UART_PORT    UART_NUM_1

/* PC server endpoint — IP/port discovered at runtime via pc_discovery */

/* ── Keyword tables ──────────────────────────────────────────────────────── */

/* "play" routes to music_request() (YouTube search path) — kept separate */
static const char *PLAYBACK_CONTROL_KEYWORDS[] = {
    "stop", "pause", "next", "previous", "volume", NULL
};

static const char *DISPLAY_KEYWORDS[] = {
    "show", "display", "screen", "clear", "update", NULL
};

/* ── Helpers ──────────────────────────────────────────────────────────────── */

static void SendUart(const char *str)
{
    uart_write_bytes(ROUTER_UART_PORT, str, strlen(str));
}

/* Case-insensitive substring search */
static int ContainsKeyword(const char *text, const char *kw)
{
    size_t tLen = strlen(text);
    size_t kLen = strlen(kw);
    if (kLen > tLen) return 0;
    for (size_t i = 0; i <= tLen - kLen; i++)
    {
        size_t j;
        for (j = 0; j < kLen; j++)
        {
            if (tolower((unsigned char)text[i + j]) !=
                tolower((unsigned char)kw[j]))  break;
        }
        if (j == kLen) return 1;
    }
    return 0;
}

/* Match transcript against a NULL-terminated keyword list */
static bool MatchesAny(const char *transcript, const char **keywords)
{
    for (int i = 0; keywords[i] != NULL; i++)
    {
        if (ContainsKeyword(transcript, keywords[i])) return true;
    }
    return false;
}

/* ── Rule A — send HTTP POST to PC server ───────────────────────────────────
 * POST http://PC_IP:PORT/command
 * Content-Type: application/json
 * Body: {"command":"play Beatles"}
 * ─────────────────────────────────────────────────────────────────────────── */
static void RouteToPC(const char *transcript)
{
    /* Build JSON body */
    char body[CLOUD_TRANSCRIPT_MAX + 32];
    snprintf(body, sizeof(body), "{\"command\":\"%s\"}", transcript);

    ESP_LOGI(TAG, "Rule A → PC: %s", body);

    char url[64];
    snprintf(url, sizeof(url), "http://%s:%u/command", pc_get_ip(), pc_get_port());
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "RouteToPC: http_client_init failed");
        return;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200)
    {
        ESP_LOGW(TAG, "PC POST failed: %s HTTP %d", esp_err_to_name(err), status);
    }
    else
    {
        ESP_LOGI(TAG, "PC POST OK");
    }
}

/* ── Rule B — send CMD: message to STM32 over UART ──────────────────────── */
static void RouteToSTM32(const char *transcript)
{
    ESP_LOGI(TAG, "Rule B → STM32 CMD: %s", transcript);

    /* Format: "CMD:<transcript>\n" — stripped by CommandHandler_Post() */
    char msg[CLOUD_TRANSCRIPT_MAX + 8];
    snprintf(msg, sizeof(msg), "CMD:%s\n", transcript);
    SendUart(msg);
}

/* ── Rule D — bare-name fallback to music search ────────────────────────────
 * If transcript doesn't match any explicit rule AND has enough characters
 * to be meaningful, treat it as a song/artist search.  Rationale: the
 * wake-word ("Hey Noa") already verifies user intent, and the 3-sec STT
 * recording window scopes the utterance, so requiring an explicit "play"
 * prefix is redundant.  Filters out very short transcripts ("the", "yes",
 * noise) to limit false-positive YouTube searches. */
#define RULE_D_MIN_NONSPACE_CHARS  3u

static bool LooksLikeMusicQuery(const char *transcript)
{
    if (!transcript) { return false; }
    size_t chars = 0;
    for (const char *p = transcript; *p; p++)
    {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') { chars++; }
        if (chars >= RULE_D_MIN_NONSPACE_CHARS) { return true; }
    }
    return false;
}

static void RouteAsMusicSearch(const char *transcript)
{
    ESP_LOGI(TAG, "Rule D → music_request (bare name): \"%s\"", transcript);
    music_request(transcript);
}

/* ── Rule C — unknown command ─────────────────────────────────────────────── */
static void RouteUnknown(const char *transcript)
{
    ESP_LOGW(TAG, "Rule C — unknown: \"%s\"", transcript);
    SendUart("CMD:UNKNOWN\n");
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

void CommandRouter_Init(void)
{
    ESP_LOGI(TAG, "command_router ready (PC will be discovered at runtime)");
}

/* ── CommandRouter_Route ─────────────────────────────────────────────────── */
void CommandRouter_Route(const char *transcript)
{
    if (!transcript || transcript[0] == '\0')
    {
        ESP_LOGW(TAG, "empty transcript — skipping");
        return;
    }

    ESP_LOGI(TAG, "routing: \"%s\"", transcript);

    if (ContainsKeyword(transcript, "play"))
    {
        /* Rule A1 — "play X": find "play " anywhere in the transcript and
         * forward only the part after it. Handles "hey Noah play Beatles"
         * → "Beatles". Falls back to full transcript if "play " not found
         * as a word boundary. */
        const char *q   = transcript;
        const char *hit = NULL;
        for (const char *p = transcript; *p; p++) {
            if ((p == transcript || p[-1] == ' ') &&
                strncasecmp(p, "play ", 5) == 0) {
                hit = p + 5;
                break;
            }
        }
        if (hit && *hit) q = hit;
        ESP_LOGI(TAG, "Rule A1 → music_request: %s", q);
        music_request(q);
    }
    else if (MatchesAny(transcript, PLAYBACK_CONTROL_KEYWORDS))
    {
        RouteToPC(transcript);          /* Rule A2 — stop/pause/next/prev/volume → PC */
    }
    else if (MatchesAny(transcript, DISPLAY_KEYWORDS))
    {
        RouteToSTM32(transcript);       /* Rule B */
    }
    else if (LooksLikeMusicQuery(transcript))
    {
        RouteAsMusicSearch(transcript); /* Rule D — bare song / artist name */
    }
    else
    {
        RouteUnknown(transcript);       /* Rule C — too short to be a query */
    }
}
