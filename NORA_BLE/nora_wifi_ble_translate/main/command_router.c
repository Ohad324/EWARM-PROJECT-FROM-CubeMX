/*
 * command_router.c — voice command keyword router (NORA/ESP32 side)
 *
 * Receives a transcript string from CloudUpload_Transcribe() and applies
 * three routing rules in order:
 *
 *   Rule A — MEDIA keywords: play, stop, pause, next, previous, volume
 *             → HTTP POST {"command":"<transcript>"} to PC server
 *
 *   Rule B — DISPLAY keywords: show, display, screen, clear, update
 *             → UART "CMD:<transcript>\n" to STM32
 *
 *   Rule C — No match
 *             → UART "CMD:UNKNOWN\n" + ESP_LOG debug print
 *
 * ASSUMPTIONS:
 *   - WiFi is connected when CommandRouter_Route() is called.
 *   - The PC server is running and reachable at PC_IP:PC_PORT.
 *   - UART_NUM_1 is already initialised by nora_ble_bridge.c (TX=17, RX=18).
 */

#include "command_router.h"
#include "cloud_upload.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *TAG = "cmd_router";

/* UART port shared with the rest of the bridge */
#define ROUTER_UART_PORT    UART_NUM_1

/* PC server endpoint */
#define PC_SERVER_URL  "http://" PC_IP ":" STRINGIFY(PC_PORT) "/command"

/* Stringify helper for PC_PORT integer → string in URL */
#define STRINGIFY_(x) #x
#define STRINGIFY(x)  STRINGIFY_(x)

/* ── Keyword tables ──────────────────────────────────────────────────────── */

static const char *MEDIA_KEYWORDS[] = {
    "play", "stop", "pause", "next", "previous", "volume", NULL
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

    esp_http_client_config_t cfg = {
        .url        = PC_SERVER_URL,
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
    ESP_LOGI(TAG, "command_router ready (PC=%s:%d)", PC_IP, PC_PORT);
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

    if (MatchesAny(transcript, MEDIA_KEYWORDS))
    {
        RouteToPC(transcript);          /* Rule A */
    }
    else if (MatchesAny(transcript, DISPLAY_KEYWORDS))
    {
        RouteToSTM32(transcript);       /* Rule B */
    }
    else
    {
        RouteUnknown(transcript);       /* Rule C */
    }
}
