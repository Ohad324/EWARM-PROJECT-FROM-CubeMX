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

/* ── Configuration ──────────────────────────────────────────────────────── */
#define MUSIC_TAG              "MUSIC"
#define YOUTUBE_API_KEY        "AIzaSyB-ClvuzNPEIwXGz_NSb0yApeRye-Gt13g"
#define YOUTUBE_SEARCH_URL     "https://www.googleapis.com/youtube/v3/search"
#define UART_PORT              UART_NUM_1
#define THUMB_STREAM_CHUNK     512  /* bytes per UART write during streaming */
#define THUMB_MIN_BYTES        5000 /* below this → YouTube placeholder image */
#define UART_MUTEX_TIMEOUT_MS  5000
#define PC_PLAYER_URL          "http://nora-player.local:5000/play"

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
        ESP_LOGW(MUSIC_TAG, "YouTube: no results found");
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

    esp_http_client_config_t cfg = {
        .url        = PC_PLAYER_URL,
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

        ESP_LOGI(MUSIC_TAG, "Searching YouTube: \"%s\"", s_query_buf);

        /* Step 3: clear result buffers */
        s_title_buf[0]    = '\0';
        s_channel_buf[0]  = '\0';
        s_video_id_buf[0] = '\0';
        s_url_buf[0]      = '\0';

        /* Step 4: YouTube search */
        if (!youtube_search()) {
            if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(UART_MUTEX_TIMEOUT_MS)) == pdTRUE) {
                uart_write_bytes(UART_PORT, "ERROR:not_found\n", 16);
                xSemaphoreGive(s_uart_mutex);
            } else {
                ESP_LOGE(MUSIC_TAG, "UART mutex timeout — error not sent");
            }
            continue;
        }

        /* Notify connected BLE device (iPhone) with YouTube URL */
        char yt_url[64];
        snprintf(yt_url, sizeof(yt_url), "https://youtu.be/%s", s_video_id_buf);
        ble_notify_send(yt_url);

        /* POST YouTube URL to PC player over Wi-Fi */
        post_to_pc_player(s_video_id_buf, s_title_buf, s_channel_buf);

        /* Step 5: take UART mutex for the full TRACK + THUMB transfer */
        /* Step 6: take UART mutex for the full TRACK + THUMB transfer */
        if (xSemaphoreTake(s_uart_mutex, pdMS_TO_TICKS(UART_MUTEX_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(MUSIC_TAG, "UART mutex timeout — transfer skipped");
            continue;
        }

        /* Step 7: send TRACK: */
        ESP_LOGI(MUSIC_TAG, "Sending TRACK: to STM32");
        char track_msg[512];
        int  tlen = snprintf(track_msg, sizeof(track_msg),
                             "TRACK:%s|%s|%s\n",
                             s_title_buf, s_channel_buf, s_video_id_buf);
        uart_write_bytes(UART_PORT, track_msg, tlen);

        /* Step 8: stream maxresdefault thumbnail — HD only.
         * thumb_stream_uart() opens the HTTP connection, sends the THUMB:
         * header, then pipes chunks straight to UART — mutex stays held
         * throughout so the binary body is contiguous on the wire.
         * Returns >0 on success, 0 for placeholder (<5000 B), <0 for error. */
        {
            char thumb_url[128];
            snprintf(thumb_url, sizeof(thumb_url),
                     "https://i.ytimg.com/vi/%s/maxresdefault.jpg",
                     s_video_id_buf);
            int r = thumb_stream_uart(thumb_url);
            if (r > 0) {
                ESP_LOGI(MUSIC_TAG, "Thumbnail OK: %d bytes", r);
            } else if (r == 0) {
                ESP_LOGW(MUSIC_TAG, "maxresdefault not HD — no thumbnail sent");
            } else {
                ESP_LOGW(MUSIC_TAG, "Thumbnail stream error — no thumbnail sent");
            }
        }

        /* Step 9: release UART mutex */
        xSemaphoreGive(s_uart_mutex);

        ESP_LOGI(MUSIC_TAG, "Transfer complete");
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
