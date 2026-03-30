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
#include "esp_log.h"
#include "cJSON.h"
#include "driver/uart.h"
#include <string.h>
#include <stdio.h>

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
            if (!esp_http_client_is_chunked_response(evt->client))
            {
                int avail = (int)sizeof(s_responseBuf) - s_responseLen - 1;
                int copy  = (evt->data_len < avail) ? evt->data_len : avail;
                memcpy(s_responseBuf + s_responseLen, evt->data, copy);
                s_responseLen += copy;
            }
            break;

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
        .skip_cert_common_name_check = false,
        /* For production: set .crt_bundle_attach = esp_crt_bundle_attach */
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
        (unsigned)31250,
        audioUri);

    esp_http_client_config_t cfg = {
        .url           = STT_API_URL,
        .method        = HTTP_METHOD_POST,
        .event_handler = HttpEventHandler,
        .timeout_ms    = 15000,   /* 15 s: synchronous STT is usually < 5 s */
        .buffer_size   = 4096,
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
