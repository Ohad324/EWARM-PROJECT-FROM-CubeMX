/*
 * cloud_upload.c — GCS upload + Google Cloud Speech-to-Text (NORA/ESP32 side)
 *
 * FLOW:
 *   1. STM32 sends "AUDIO:FILE:REC_001.wav:<byteCount>\n" then raw WAV bytes.
 *   2. nora_ble_bridge.c receives and accumulates them into a PSRAM buffer.
 *   3. nora_ble_bridge.c calls CloudUpload_UploadWav() with the buffer.
 *   4. CloudUpload_UploadWav() HTTP PUTs the bytes to GCS XML API.
 *   5. On success, CloudUpload_Transcribe() calls Speech-to-Text API using
 *      the gs:// URI of the uploaded file (avoids sending audio twice).
 *   6. The transcript is returned to nora_ble_bridge.c for routing.
 *
 * GCS AUTHENTICATION:
 *   Uses a pre-generated OAuth2 Bearer token stored as CLOUD_GCS_BEARER_TOKEN.
 *   Tokens are valid for ~1 hour. For production, implement token refresh using
 *   a service-account private key and the JWT grant flow.
 *
 * SPEECH-TO-TEXT:
 *   Uses the synchronous REST API (v1/speech:recognize) with a gs:// audio URI.
 *   Maximum audio duration: 60 seconds for synchronous recognition.
 *   For longer recordings, switch to longrunningrecognize and poll for results.
 *
 * DEPENDENCIES (esp-idf components):
 *   - esp_http_client (idf component)
 *   - cJSON (bundled with esp-idf)
 *   - WiFi must be connected before calling any function here
 */

#include "cloud_upload.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>

#define STREAM_CHUNK_SIZE  1024u   /* UART read + HTTP write chunk size */

static const char *TAG = "cloud_upload";

/* UART port that connects to STM32 (same as the rest of nora_ble_bridge.c) */
#define CLOUD_UART_PORT   UART_NUM_1
#define CLOUD_UART_TIMEOUT_MS  100

/* GCS XML API base URL: PUT /<bucket>/<object> with Bearer token */
#define GCS_UPLOAD_URL_FMT \
    "https://storage.googleapis.com/%s/%s"

/* Speech-to-Text REST API */
#define STT_API_URL \
    "https://speech.googleapis.com/v1/speech:recognize?key=" SPEECH_API_KEY

/* Maximum size of the STT JSON response — typically < 2 KB */
#define STT_RESPONSE_BUF_SIZE  4096u

/* ── Module-level state ──────────────────────────────────────────────────── */

/* Response accumulation buffer — reused across requests to avoid heap churn */
static char s_responseBuf[STT_RESPONSE_BUF_SIZE];
static int  s_responseLen;

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static void SendUart(const char *str)
{
    uart_write_bytes(CLOUD_UART_PORT, str, strlen(str));
}

/* HTTP event handler — accumulates response body into s_responseBuf */
static esp_err_t HttpEventHandler(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_CONNECTED:
            s_responseLen = 0;
            memset(s_responseBuf, 0, sizeof(s_responseBuf));
            break;

        case HTTP_EVENT_ON_DATA:
        {
            int avail = (int)sizeof(s_responseBuf) - s_responseLen - 1;
            int copy  = (evt->data_len < avail) ? evt->data_len : avail;
            if (copy > 0)
            {
                memcpy(s_responseBuf + s_responseLen, evt->data, copy);
                s_responseLen += copy;
            }
            break;
        }

        default:
            break;
    }
    return ESP_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Public API                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* ── CloudUpload_Init ────────────────────────────────────────────────────── */
void CloudUpload_Init(void)
{
    /* Nothing to initialise at module level — esp_http_client is configured
     * per-request.  This function exists as a hook for future TLS pre-loading
     * or connection pooling. */
    ESP_LOGI(TAG, "cloud_upload ready (bucket=%s)", CLOUD_GCS_BUCKET);
}

/* ── CloudUpload_UploadWav ───────────────────────────────────────────────────
 * HTTP PUT the WAV buffer to Google Cloud Storage (XML API).
 *
 * The GCS XML API accepts:
 *   PUT https://storage.googleapis.com/<bucket>/<object>
 *   Authorization: Bearer <token>
 *   Content-Type: audio/wav
 *   Content-Length: <len>
 *   Body: <raw WAV bytes>
 *
 * Returns true on HTTP 200 or 200-range status.
 * ─────────────────────────────────────────────────────────────────────────── */
bool CloudUpload_UploadWav(const char *filename, const uint8_t *data, size_t len)
{
    char url[256];
    snprintf(url, sizeof(url), GCS_UPLOAD_URL_FMT, CLOUD_GCS_BUCKET, filename);

    esp_http_client_config_t cfg = {
        .url              = url,
        .method           = HTTP_METHOD_PUT,
        .event_handler    = HttpEventHandler,
        .timeout_ms       = 30000,   /* 30 s: large WAV files may take several seconds */
        .buffer_size      = 4096,
        .buffer_size_tx   = 4096,
        .crt_bundle_attach    = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "http_client_init failed");
        SendUart("UPLOAD:FAIL:");
        SendUart(filename);
        SendUart("\n");
        return false;
    }

    /* Bucket is public — no Authorization header needed for upload */
    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    /* Provide the body */
    esp_http_client_set_post_field(client, (const char *)data, (int)len);

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || (status < 200 || status >= 300))
    {
        ESP_LOGE(TAG, "GCS upload failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        SendUart("UPLOAD:FAIL:");
        SendUart(filename);
        SendUart("\n");
        return false;
    }

    ESP_LOGI(TAG, "GCS upload OK: %s (%zu bytes, HTTP %d)", filename, len, status);
    SendUart("UPLOAD:OK:");
    SendUart(filename);
    SendUart("\n");
    return true;
}

/* ── CloudUpload_Transcribe ──────────────────────────────────────────────────
 * Call Google Cloud Speech-to-Text on a file already in GCS.
 *
 * Request body (JSON):
 * {
 *   "config": {
 *     "encoding":        "LINEAR16",
 *     "sampleRateHertz": 31250,
 *     "languageCode":    "en-US",
 *     "model":           "command_and_search"
 *   },
 *   "audio": {
 *     "uri": "gs://<bucket>/<filename>"
 *   }
 * }
 *
 * Response (abbreviated):
 * {
 *   "results": [{ "alternatives": [{ "transcript": "play Beatles", ... }] }]
 * }
 * ─────────────────────────────────────────────────────────────────────────── */
bool CloudUpload_Transcribe(const char *filename,
                            char       *out_transcript,
                            size_t      maxLen)
{
    /* Build the gs:// URI for the audio file */
    char audioUri[128];
    snprintf(audioUri, sizeof(audioUri), "gs://%s/%s",
             CLOUD_GCS_BUCKET, filename);

    /* Build JSON request body */
    char body[512];
    snprintf(body, sizeof(body),
        "{"
            "\"config\":{"
                "\"encoding\":\"LINEAR16\","
                "\"sampleRateHertz\":%u,"
                "\"languageCode\":\"en-US\","
                "\"model\":\"command_and_search\""
            "},"
            "\"audio\":{"
                "\"uri\":\"%s\""
            "}"
        "}",
        (unsigned)16000,    /* tell Google STT 16000 Hz; actual DFSDM ~16666 Hz — STT resamples */
        audioUri);

    esp_http_client_config_t cfg = {
        .url               = STT_API_URL,
        .method            = HTTP_METHOD_POST,
        .event_handler     = HttpEventHandler,
        .timeout_ms        = 15000,   /* 15 s: synchronous STT is usually < 5 s */
        .buffer_size       = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "STT http_client_init failed");
        SendUart("STT:FAIL\n");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200)
    {
        ESP_LOGE(TAG, "STT request failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        SendUart("STT:FAIL\n");
        return false;
    }

    /* Parse JSON response to extract the transcript */
    cJSON *root = cJSON_Parse(s_responseBuf);
    if (!root)
    {
        ESP_LOGE(TAG, "STT JSON parse failed");
        SendUart("STT:FAIL\n");
        return false;
    }

    bool ok = false;

    /* Navigate: root → results[0] → alternatives[0] → transcript */
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (cJSON_IsArray(results) && cJSON_GetArraySize(results) > 0)
    {
        cJSON *first = cJSON_GetArrayItem(results, 0);
        cJSON *alts  = cJSON_GetObjectItem(first, "alternatives");
        if (cJSON_IsArray(alts) && cJSON_GetArraySize(alts) > 0)
        {
            cJSON *alt0       = cJSON_GetArrayItem(alts, 0);
            cJSON *transcript = cJSON_GetObjectItem(alt0, "transcript");
            if (cJSON_IsString(transcript) && transcript->valuestring)
            {
                snprintf(out_transcript, maxLen, "%s", transcript->valuestring);
                ESP_LOGI(TAG, "Transcript: \"%s\"", out_transcript);
                ok = true;
            }
        }
    }

    cJSON_Delete(root);

    if (!ok)
    {
        ESP_LOGW(TAG, "STT returned no transcript");
        SendUart("STT:FAIL\n");
    }

    return ok;
}

/* ── CloudUpload_StreamWav ───────────────────────────────────────────────────
 * BUFFERED upload: drain UART entirely into heap RAM first (frees STM32
 * immediately), then upload to GCS at whatever pace WiFi allows.
 *
 * Why this beats streaming-through-HTTP:
 *   The previous design wrote each UART chunk directly to esp_http_client_write.
 *   At marginal RSSI (~-90 dBm), HTTP writes block for seconds, the UART read
 *   loop stalls, NORA's UART RX buffer fills, ESP32 driver silently drops bytes,
 *   STM32 thinks all 96 KB went out but NORA only got ~85-95 KB → timeout.
 *
 *   Decoupling: read UART -> heap (fast, no HTTPS in the loop = no back-pressure
 *   = no buffer overflow possible). Once all 96 KB is in RAM, STM32 is free to
 *   move on. Then upload the buffer to GCS at whatever speed the link supports.
 *
 * Memory: NORA has ~250 KB free heap; 96 KB recording uses ~38%. Bumped against
 * AUDIO_FILE_MAX_BYTES (4 MB) sanity cap defined in nora_ble_bridge.c.
 *
 *   filename : GCS object name (e.g. "REC_001.wav")
 *   uart_port: UART port receiving WAV bytes from STM32
 *   fileSize : exact byte count declared in AUDIO:FILE: header
 *
 * Returns true on HTTP 200/201, false on any error.
 * ─────────────────────────────────────────────────────────────────────────── */
bool CloudUpload_StreamWav(const char *filename, int uart_port, uint32_t fileSize)
{
    if (fileSize == 0 || fileSize > 1024u * 1024u)
    {
        ESP_LOGE(TAG, "StreamWav: invalid fileSize=%lu", (unsigned long)fileSize);
        SendUart("UPLOAD:FAIL:"); SendUart(filename); SendUart("\n");
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc(fileSize);
    if (!buf)
    {
        ESP_LOGE(TAG, "StreamWav: malloc(%lu) failed", (unsigned long)fileSize);
        SendUart("UPLOAD:FAIL:"); SendUart(filename); SendUart("\n");
        return false;
    }

    /* Tell STM32 to start streaming NOW. No HTTP yet — STM32's bytes go
     * straight from UART RX FIFO into our heap buffer at full UART speed. */
    SendUart("AUDIO:READY\n");
    ESP_LOGI(TAG, "StreamWav: AUDIO:READY sent — buffering %lu bytes from UART",
             (unsigned long)fileSize);

    uint32_t got_total = 0;
    while (got_total < fileSize)
    {
        uint32_t toRead = fileSize - got_total;
        if (toRead > STREAM_CHUNK_SIZE) toRead = STREAM_CHUNK_SIZE;

        int got = uart_read_bytes(uart_port, buf + got_total, (size_t)toRead,
                                  pdMS_TO_TICKS(30000));
        if (got <= 0)
        {
            ESP_LOGE(TAG, "StreamWav: UART timeout at %lu/%lu",
                     (unsigned long)got_total, (unsigned long)fileSize);
            free(buf);
            SendUart("UPLOAD:FAIL:"); SendUart(filename); SendUart("\n");
            return false;
        }
        got_total += (uint32_t)got;
    }
    ESP_LOGI(TAG, "StreamWav: %lu bytes buffered — opening GCS HTTPS",
             (unsigned long)got_total);

    /* All bytes are now in RAM. STM32's UART transfer is finished — it's free
     * to handle the next button press while we upload to GCS. */
    char url[256];
    snprintf(url, sizeof(url), GCS_UPLOAD_URL_FMT, CLOUD_GCS_BUCKET, filename);

    esp_http_client_config_t cfg = {
        .url                  = url,
        .method               = HTTP_METHOD_PUT,
        .timeout_ms           = 60000,
        .buffer_size          = 4096,
        .buffer_size_tx       = 4096,
        .crt_bundle_attach    = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "StreamWav: http_client_init failed");
        free(buf);
        SendUart("UPLOAD:FAIL:"); SendUart(filename); SendUart("\n");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "audio/wav");

    esp_err_t err = esp_http_client_open(client, (int)fileSize);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "StreamWav: open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buf);
        SendUart("UPLOAD:FAIL:"); SendUart(filename); SendUart("\n");
        return false;
    }

    uint32_t up_sent = 0;
    bool     ok      = true;
    while (up_sent < fileSize)
    {
        uint32_t toWrite = fileSize - up_sent;
        if (toWrite > STREAM_CHUNK_SIZE) toWrite = STREAM_CHUNK_SIZE;

        int written = esp_http_client_write(client,
                                            (const char *)(buf + up_sent),
                                            (int)toWrite);
        if (written < 0)
        {
            ESP_LOGE(TAG, "StreamWav: HTTP write failed at %lu/%lu",
                     (unsigned long)up_sent, (unsigned long)fileSize);
            ok = false;
            break;
        }
        up_sent += (uint32_t)written;
    }

    free(buf);   /* release RAM as soon as upload body is sent */

    int status = 0;
    if (ok)
    {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ok = (status >= 200 && status < 300);
    }
    esp_http_client_cleanup(client);

    if (!ok)
    {
        ESP_LOGE(TAG, "StreamWav: failed — uploaded=%lu/%lu HTTP=%d",
                 (unsigned long)up_sent, (unsigned long)fileSize, status);
        SendUart("UPLOAD:FAIL:"); SendUart(filename); SendUart("\n");
        return false;
    }

    ESP_LOGI(TAG, "StreamWav: OK — %lu bytes uploaded to GCS, HTTP %d",
             (unsigned long)up_sent, status);
    SendUart("UPLOAD:OK:"); SendUart(filename); SendUart("\n");
    return true;
}
