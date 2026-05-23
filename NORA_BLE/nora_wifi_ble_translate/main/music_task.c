/*
 * music_task.c
 * YouTube Search + Thumbnail — NORA ESP32-S3 music feature
 *
 * Public API:
 *   music_task_init(uart_mutex)  — call once from app_main after UART mutex created
 *   music_request(query)         — call from BLE handler on "PLAY:" command
 *
 * UART protocol to STM32:
 *   TRACK:<title>|<channel>|<videoId>\n
 *   THUMB:<size>\n  + <size> raw JPEG bytes (512-byte chunks, 10ms gap after header)
 *   ERROR:not_found\n / ERROR:youtube_fail\n  on failure
 */
#include "music_task.h"
#include "nora_ble_bridge.h"
#include "pc_discovery.h"
#include "ntfy_client.h"

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

/* Build "http://<ip>:<port>/play" into a caller-supplied buffer */
static void pc_player_url(char *buf, size_t buflen)
{
    snprintf(buf, buflen, "http://%s:%u/play", pc_get_ip(), pc_get_port());
}

/* ── Configuration ──────────────────────────────────────────────────────── */
#define MUSIC_TAG              "MUSIC"
#define YOUTUBE_API_KEY        ""  /* leave empty to skip direct API and use PC fallback */
#define YOUTUBE_SEARCH_URL     "https://www.googleapis.com/youtube/v3/search"
#define UART_PORT              UART_NUM_1
#define THUMB_STREAM_CHUNK     512  /* bytes per UART write during streaming */
#define THUMB_MIN_BYTES        5000 /* below this → YouTube placeholder image */
#define UART_MUTEX_TIMEOUT_MS  5000
/* PC_PLAYER_URL built at runtime via pc_discovery — see pc_player_url() */

/* ── Request struct ─────────────────────────────────────────────────────── */
typedef struct {
    char query[256];
} music_request_t;

/* ── Static buffers ─────────────────────────────────────────────────────── */
/* No large thumbnail buffer — thumbnail is streamed chunk-by-chunk from    *
 * HTTP directly to UART, so only a small stack buffer is needed.           */
static char    s_title_buf[256];     /* song title UTF-8                     */
static char    s_channel_buf[128];   /* channel / artist name                */
static char    s_video_id_buf[32];   /* YouTube video ID                     */
static char    s_url_buf[512];       /* thumbnail URL (maxresdefault)        */
static char    s_query_buf[256];     /* current search query                 */
static char    s_json_buf[16384];    /* YouTube API JSON response            */

/* ── Module-private handles ─────────────────────────────────────────────── */
static QueueHandle_t     s_music_queue;
static SemaphoreHandle_t s_uart_mutex;

/* ── URL encoder ────────────────────────────────────────────────────────── */
static void url_encode(const char *in, char *out, size_t out_len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i = 0;
    while (*in && i + 4 < out_len) {
        unsigned char c = (unsigned char)*in++;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[i++] = (char)c;
        } else {
            out[i++] = '%';
            out[i++] = hex[c >> 4];
            out[i++] = hex[c & 0x0F];
        }
    }
    out[i] = '\0';
}

/* ── YouTube Data API v3 search ─────────────────────────────────────────── *
 * Reads from s_query_buf.                                                   *
 * Fills s_video_id_buf, s_title_buf, s_channel_buf, s_url_buf on success.  *
 * Returns true on success, false on HTTP error / no results / parse error.  *
 * ─────────────────────────────────────────────────────────────────────── */
static bool youtube_search(void)
{
    char encoded[768];
    url_encode(s_query_buf, encoded, sizeof(encoded));

    char url[1024];
    snprintf(url, sizeof(url),
             "%s?part=snippet&q=%s&type=video&maxResults=1&key=%s"
             "&fields=items(id/videoId,snippet/title,snippet/channelTitle,snippet/thumbnails/medium/url)",
             YOUTUBE_SEARCH_URL, encoded, YOUTUBE_API_KEY);

    esp_http_client_config_t config = {
        .url                         = url,
        .timeout_ms                  = 10000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(MUSIC_TAG, "HTTP client init failed");
        return false;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(MUSIC_TAG, "YouTube HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(MUSIC_TAG, "YouTube response: HTTP %d — parsing JSON", status);

    int read = 0, n;
    while ((n = esp_http_client_read(client, s_json_buf + read,
                                     (int)sizeof(s_json_buf) - 1 - read)) > 0)
        read += n;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read <= 0) {
        ESP_LOGE(MUSIC_TAG, "YouTube read failed (read=%d)", read);
        return false;
    }
    s_json_buf[read] = '\0';

    cJSON *root = cJSON_Parse(s_json_buf);
    if (!root) {
        ESP_LOGE(MUSIC_TAG, "YouTube JSON parse failed");
        return false;
    }

    bool ok = false;
    cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!items || cJSON_GetArraySize(items) == 0) {
        ESP_LOGW(MUSIC_TAG, "YouTube: no results found — body: %.200s", s_json_buf);
        cJSON_Delete(root);
        return false;
    }

    cJSON *item0   = cJSON_GetArrayItem(items, 0);
    cJSON *id      = cJSON_GetObjectItem(item0, "id");
    cJSON *snippet = cJSON_GetObjectItem(item0, "snippet");

    if (id && snippet) {
        cJSON *jVideoId  = cJSON_GetObjectItem(id,      "videoId");
        cJSON *jTitle    = cJSON_GetObjectItem(snippet, "title");
        cJSON *jChannel  = cJSON_GetObjectItem(snippet, "channelTitle");
        cJSON *jThumbs   = cJSON_GetObjectItem(snippet, "thumbnails");
        cJSON *jMedium   = jThumbs ? cJSON_GetObjectItem(jThumbs, "medium") : NULL;
        cJSON *jThumbUrl = jMedium ? cJSON_GetObjectItem(jMedium, "url")    : NULL;

        if (jVideoId && cJSON_IsString(jVideoId)) {
            strncpy(s_video_id_buf, jVideoId->valuestring, sizeof(s_video_id_buf) - 1);
            ESP_LOGI(MUSIC_TAG, "Found: videoId=%s", s_video_id_buf);
            ok = true;
        }
        if (jTitle && cJSON_IsString(jTitle)) {
            strncpy(s_title_buf, jTitle->valuestring, sizeof(s_title_buf) - 1);
            ESP_LOGI(MUSIC_TAG, "Title: %s", s_title_buf);
        }
        if (jChannel && cJSON_IsString(jChannel)) {
            strncpy(s_channel_buf, jChannel->valuestring, sizeof(s_channel_buf) - 1);
            ESP_LOGI(MUSIC_TAG, "Channel: %s", s_channel_buf);
        }
        if (jThumbUrl && cJSON_IsString(jThumbUrl)) {
            strncpy(s_url_buf, jThumbUrl->valuestring, sizeof(s_url_buf) - 1);
            ESP_LOGI(MUSIC_TAG, "Thumbnail URL: %s", s_url_buf);
        }
    }

    cJSON_Delete(root);
    return ok;
}

/* ── Thumbnail stream ───────────────────────────────────────────────────── *
 * Opens HTTP connection to url, validates the JPEG header, then streams    *
 * the body directly to UART in 512-byte chunks — no large buffer.          *
 *                                                                           *
 * Must be called with s_uart_mutex already held by the caller.             *
 * Returns total bytes streamed on success,                                  *
 *          0 if the image is a YouTube placeholder (< THUMB_MIN_BYTES),    *
 *         -1 on any other error.                                            *
 * ─────────────────────────────────────────────────────────────────────── */
static int thumb_stream_uart(const char *url)
{
    ESP_LOGI(MUSIC_TAG, "Streaming thumbnail -> UART: %s", url);

    esp_http_client_config_t cfg = {
        .url                         = url,
        .timeout_ms                  = 15000,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .max_redirection_count       = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(MUSIC_TAG, "Thumbnail HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return -1;
    }

    int content_len = (int)esp_http_client_fetch_headers(client);
    ESP_LOGI(MUSIC_TAG, "Thumbnail Content-Length: %d bytes", content_len);

    /* Reject YouTube placeholder images — they are ~1097 bytes */
    if (content_len > 0 && content_len < THUMB_MIN_BYTES) {
        ESP_LOGW(MUSIC_TAG, "NOT HD: content_len=%d B (< %d) — placeholder detected",
                 content_len, THUMB_MIN_BYTES);
        ESP_LOGW(MUSIC_TAG, "NOT HD URL: %s", url);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return 0;
    }

    /* Validate JPEG SOI marker (first two bytes must be 0xFF 0xD8) */
    uint8_t soi[16];  /* read 16 bytes so we can log the full JPEG preamble */
    int soi_read = esp_http_client_read(client, (char *)soi, sizeof(soi));
    if (soi_read < 2 || soi[0] != 0xFF || soi[1] != 0xD8) {
        ESP_LOGW(MUSIC_TAG, "Not a valid JPEG — first bytes:");
        if (soi_read >= 8)
            ESP_LOGW(MUSIC_TAG, "  %02X %02X %02X %02X %02X %02X %02X %02X",
                     soi[0],soi[1],soi[2],soi[3],soi[4],soi[5],soi[6],soi[7]);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }
    ESP_LOGI(MUSIC_TAG, "JPEG SOI OK — first 16 bytes: "
             "%02X %02X %02X %02X %02X %02X %02X %02X "
             "%02X %02X %02X %02X %02X %02X %02X %02X",
             soi[0],soi[1],soi[2],soi[3],soi[4],soi[5],soi[6],soi[7],
             soi[8],soi[9],soi[10],soi[11],soi[12],soi[13],soi[14],soi[15]);

    /* If server didn't send Content-Length, read fully to learn the size.
     * This is rare for YouTube CDN but handled gracefully.                 */
    if (content_len <= 0) {
        ESP_LOGW(MUSIC_TAG, "No Content-Length — cannot stream, skipping thumb");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return -1;
    }

    /* Send THUMB: header over UART (mutex already held by caller) */
    char thumb_hdr[32];
    int  hlen = snprintf(thumb_hdr, sizeof(thumb_hdr), "THUMB:%d\n", content_len);
    uart_write_bytes(UART_PORT, thumb_hdr, hlen);
    ESP_LOGI(MUSIC_TAG, "THUMB: header sent (%d bytes total)", content_len);

    /* 10 ms gap — gives STM32 time to switch from ASCII to binary mode */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Stream: SOI+preamble first, then remainder in 512-byte chunks */
    uart_write_bytes(UART_PORT, (const char *)soi, soi_read);
    int total = soi_read;
    int next_log = 10240;  /* log progress every 10 KB */

    uint8_t chunk[512];
    int n;
    while ((n = esp_http_client_read(client, (char *)chunk, sizeof(chunk))) > 0)
    {
        uart_write_bytes(UART_PORT, (const char *)chunk, n);
        total += n;
        if (total >= next_log) {
            ESP_LOGI(MUSIC_TAG, "  ... streamed %d / %d bytes (%.0f%%)",
                     total, content_len, (100.0f * total) / content_len);
            next_log += 10240;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total == content_len)
        ESP_LOGI(MUSIC_TAG, "Thumbnail stream COMPLETE: %d bytes — ALL OK", total);
    else
        ESP_LOGW(MUSIC_TAG, "Thumbnail stream INCOMPLETE: got %d / %d bytes (%d missing)",
                 total, content_len, content_len - total);
    return total;
}

/* ── HTTP POST YouTube URL to PC player ─────────────────────────────────── *
 * POSTs {"url":"https://youtu.be/<id>"} to PC_PLAYER_URL.                  *
 * Non-fatal — logs error but does not stop the rest of the flow.           *
 * ─────────────────────────────────────────────────────────────────────── */
static void post_to_pc_player(const char *video_id, const char *title, const char *artist)
{
    char body[640];
    snprintf(body, sizeof(body),
             "{\"videoId\":\"%s\",\"title\":\"%s\",\"artist\":\"%s\"}",
             video_id, title, artist);

    char url[64];
    pc_player_url(url, sizeof(url));
    esp_http_client_config_t cfg = {
        .url        = url,
        .timeout_ms = 1000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(MUSIC_TAG, "PC player: HTTP client init failed");
        return;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_log_level_set("esp-tls",        ESP_LOG_NONE);
    esp_log_level_set("transport_base", ESP_LOG_NONE);
    esp_log_level_set("HTTP_CLIENT",    ESP_LOG_NONE);

    esp_err_t err = esp_http_client_perform(client);

    esp_log_level_set("esp-tls",        ESP_LOG_WARN);
    esp_log_level_set("transport_base", ESP_LOG_WARN);
    esp_log_level_set("HTTP_CLIENT",    ESP_LOG_WARN);

    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(MUSIC_TAG, "PC player: POST OK (HTTP %d) → %s", status, body);
    } else {
        ESP_LOGE(MUSIC_TAG, "PC player: POST failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/* ── Ask PC player to search + open Chrome, return resolved videoId ─────── *
 * POSTs {"query":"..."} to PC_PLAYER_URL.                                   *
 * PC player scrapes YouTube (no API key), opens Chrome, returns:            *
 *   {"status":"ok","videoId":"xxxxxxxxxxx"}                                 *
 * On success: fills s_video_id_buf, s_title_buf, s_url_buf (ytimg CDN).     *
 * Returns true if a videoId was resolved and thumbnail URL is ready.        *
 * ─────────────────────────────────────────────────────────────────────── */
static bool search_via_pc(const char *query)
{
    /* Replaced 2026-05-23: was LAN HTTP POST to pc_player_url().  iPhone
     * hotspot client-isolation blocks LAN traffic between hotspot clients
     * (BUG-004), so we now relay through ntfy.sh — both NORA and the PC
     * bridge make outgoing connections only, which the iPhone permits.
     *
     * Requires bridge.py to be running on the PC.  See
     * C:\Tools\ntfy-music-bridge\README.md. */
    if (!ntfy_search(query, s_video_id_buf, sizeof(s_video_id_buf), 15000))
    {
        ESP_LOGW(MUSIC_TAG, "search_via_ntfy: no result for \"%s\"", query);
        return false;
    }

    /* Thumbnail direct from YouTube CDN — no API key needed */
    snprintf(s_url_buf, sizeof(s_url_buf),
             "https://img.youtube.com/vi/%s/mqdefault.jpg", s_video_id_buf);
    /* Use query as title since the PC scrape returns only the videoId */
    strncpy(s_title_buf, query, sizeof(s_title_buf) - 1);
    s_title_buf[sizeof(s_title_buf) - 1] = '\0';
    s_channel_buf[0] = '\0';

    ESP_LOGI(MUSIC_TAG, "search_via_ntfy: videoId=%s  thumb=%s",
             s_video_id_buf, s_url_buf);
    return true;
}

/* ── Music FreeRTOS task ────────────────────────────────────────────────── */
static void music_task(void *param)
{
    music_request_t req;
    ESP_LOGI(MUSIC_TAG, "Music task started");

    while (1) {
        /* Step 1: wait for request */
        if (xQueueReceive(s_music_queue, &req, portMAX_DELAY) != pdTRUE) continue;

        /* Step 2: copy query to static buffer, trim leading whitespace */
        strncpy(s_query_buf, req.query, sizeof(s_query_buf) - 1);
        s_query_buf[sizeof(s_query_buf) - 1] = '\0';
        {
            char *q = s_query_buf;
            while (*q == ' ') q++;
            if (q != s_query_buf) memmove(s_query_buf, q, strlen(q) + 1);
        }

        ESP_LOGI(MUSIC_TAG, "BLE search mode: \"%s\"", s_query_buf);

        /* Step A: find the YouTube video for this query.
         *
         * Option 1 — YouTube Data API (fast, no Chrome needed):
         *   Sends the query directly to Google's API and gets back the
         *   videoId, title, channel, and thumbnail URL in one HTTPS call.
         *   Requires a valid API key in YOUTUBE_API_KEY.
         *   If the key is empty or the call fails → fall through to Option 2.
         *
         * Option 2 — PC fallback (no API key needed):
         *   POSTs {"query":"..."} to the Python server running on the PC.
         *   The PC server scrapes YouTube, opens Chrome with the video,
         *   and returns {"status":"ok","videoId":"..."}.
         *   We then build the thumbnail URL from the videoId directly
         *   (YouTube CDN: img.youtube.com/vi/<id>/mqdefault.jpg).
         *
         * Once we have the videoId + thumbnail URL the rest of the flow
         * is the same regardless of which option succeeded.
         */
        bool found            = false;
        bool used_pc_fallback = false;  /* PC already opened Chrome → skip second POST */

        /* Try the YouTube API only if a key has been configured */
        if (strlen(YOUTUBE_API_KEY) > 0)
            found = youtube_search();   /* fills s_video_id_buf, s_url_buf on success */

        /* If API is not configured or returned an error — ask the PC */
        if (!found) {
            found = search_via_pc(s_query_buf);  /* PC opens Chrome, returns videoId */
            used_pc_fallback = found;
        }

        /* Step B: send result to STM32 over UART.
         * This is independent of the PC — happens whether or not the PC is still busy. */
        if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(UART_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(MUSIC_TAG, "UART mutex timeout — TRACK not sent");
            continue;
        }

        char track_msg[512];
        int  tlen;
        if (found) {
            tlen = snprintf(track_msg, sizeof(track_msg),
                            "TRACK:%s|%s|%s\n",
                            s_title_buf, s_channel_buf, s_video_id_buf);
        } else {
            tlen = snprintf(track_msg, sizeof(track_msg), "TRACK:%s||\n", s_query_buf);
        }
        uart_write_bytes(UART_PORT, track_msg, tlen);
        ESP_LOGI(MUSIC_TAG, "TRACK sent: %.*s", tlen - 1, track_msg);

        bool thumb_sent = false;
        if (found && s_url_buf[0] != '\0') {
            int bytes = thumb_stream_uart(s_url_buf);
            if (bytes > 0) {
                ESP_LOGI(MUSIC_TAG, "THUMB sent: %d bytes", bytes);
                thumb_sent = true;
            } else {
                ESP_LOGW(MUSIC_TAG, "THUMB skipped (bytes=%d)", bytes);
            }
        } else {
            ESP_LOGW(MUSIC_TAG, "THUMB NOT sent — found=%d url=%s",
                     (int)found, s_url_buf[0] ? s_url_buf : "(empty)");
        }

        xSemaphoreGive(s_uart_mutex);
        ESP_LOGI(MUSIC_TAG, "Done — TRACK sent, THUMB %s",
                 thumb_sent ? "SENT" : "NOT SENT");

        /* Step C: tell the PC player to open Chrome with this video — AFTER STM32 is done.
         * Only needed when the YouTube API path was used (Option 1 above).
         * When the PC fallback (Option 2) was used, Chrome is already open — skip,
         * otherwise the second POST kills the running Chrome and replays. */
        if (found && !used_pc_fallback && s_video_id_buf[0] != '\0')
            post_to_pc_player(s_video_id_buf, s_title_buf, s_channel_buf);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void music_task_init(SemaphoreHandle_t uart_mutex)
{
    s_uart_mutex  = uart_mutex;
    s_music_queue = xQueueCreate(2, sizeof(music_request_t));
    assert(s_music_queue);

    xTaskCreatePinnedToCore(music_task, "music_task", 8192, NULL, 5, NULL, APP_CPU_NUM);
    ESP_LOGI(MUSIC_TAG, "Music task initialized");
}

void music_request(const char *query)
{
    music_request_t req;
    strncpy(req.query, query, sizeof(req.query) - 1);
    req.query[sizeof(req.query) - 1] = '\0';

    if (xQueueSend(s_music_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(MUSIC_TAG, "Music queue full — request dropped: '%s'", query);
    } else {
        ESP_LOGI(MUSIC_TAG, "PLAY command queued: '%s'", query);
    }
}
