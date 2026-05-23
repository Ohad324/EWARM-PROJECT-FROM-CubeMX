/*
 * ntfy_client.c — NORA → PC music-search bridge via ntfy.sh
 *
 * See ntfy_client.h for protocol overview.  Uses the same esp_http_client
 * + esp_crt_bundle stack already initialised for GCS/STT — no new
 * dependencies, no new TLS roots required.
 *
 * Topic names live as #defines in this file.  To rotate them (e.g. after
 * leaking the .bin), change both NTFY_TOPIC_REQ and NTFY_TOPIC_RES here
 * AND in the PC bridge.py REQ_TOPIC/RES_TOPIC constants.
 */
#include "ntfy_client.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>

/* ── Configuration ──────────────────────────────────────────────────────── */

/* Topics — random/unguessable so we don't collide with anyone on the
 * public ntfy.sh service.  Treat them like a shared secret.            */
#ifndef NTFY_TOPIC_REQ
#define NTFY_TOPIC_REQ  "hey-noa-7f3a9b2c-req"
#endif
#ifndef NTFY_TOPIC_RES
#define NTFY_TOPIC_RES  "hey-noa-7f3a9b2c-res"
#endif

#define NTFY_URL_PUB    "https://ntfy.sh/" NTFY_TOPIC_REQ

/* Poll URL uses ntfy.sh's `since=Ns` duration syntax — relative to the
 * server's clock, NOT NORA's `time(NULL)`.  Required because NORA may
 * boot before SNTP sync, in which case time(NULL) returns ~0 and the
 * poll would pull every message ever sent on the topic. */
#define NTFY_SINCE_WINDOW    "20s"     /* 15 s NORA timeout + 5 s margin */
#define NTFY_URL_POLL       "https://ntfy.sh/" NTFY_TOPIC_RES \
                            "/json?since=" NTFY_SINCE_WINDOW "&poll=1"

#define NTFY_POLL_INTERVAL_MS  250u    /* gap between polls (PC bridge ~30 ms) */
#define NTFY_RESP_BUF_SIZE     2048u   /* one poll response buffer */

/* ── Module state ───────────────────────────────────────────────────────── */

static const char *TAG = "ntfy";

/* Single-shot response accumulator — only one ntfy_search() in flight at
 * a time (called from music_task), so a single static buffer is safe. */
static char s_respBuf[NTFY_RESP_BUF_SIZE];
static int  s_respLen;

/* ── Internal helpers ───────────────────────────────────────────────────── */

/* TODO(robustness): HTTP_EVENT_ON_CONNECTED isn't guaranteed to fire
 * before every ON_DATA across retries/redirects.  Defensive option is
 * to also reset s_respLen=0 just before each esp_http_client_perform()
 * call.  Not changed today because all observed traffic is single-shot
 * POST/GET with no redirect chain (ntfy.sh terminates HTTPS itself).  */
static esp_err_t http_event(esp_http_client_event_t *evt)
{
    switch (evt->event_id)
    {
        case HTTP_EVENT_ON_CONNECTED:
            s_respLen = 0;
            memset(s_respBuf, 0, sizeof(s_respBuf));
            break;

        case HTTP_EVENT_ON_DATA:
        {
            int avail = (int)sizeof(s_respBuf) - s_respLen - 1;
            int copy  = (evt->data_len < avail) ? evt->data_len : avail;
            if (copy > 0)
            {
                memcpy(s_respBuf + s_respLen, evt->data, copy);
                s_respLen += copy;
            }
            break;
        }

        default:
            break;
    }
    return ESP_OK;
}

/* Publish {"id":N,"query":"..."} to the request topic.
 * Returns true on HTTP 2xx.
 *
 * Body is built with cJSON so the query string is properly escaped —
 * Google STT can return any UTF-8 including ", \, control chars, which
 * would break a raw snprintf("...\"%s\"..."). */
static bool publish_request(uint32_t req_id, const char *query)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) { ESP_LOGE(TAG, "publish: cJSON_CreateObject failed"); return false; }
    cJSON_AddNumberToObject(root, "id",    (double)req_id);
    cJSON_AddStringToObject(root, "query", query);

    char body[256];
    if (!cJSON_PrintPreallocated(root, body, (int)sizeof(body), /*fmt=*/0))
    {
        ESP_LOGE(TAG, "publish: cJSON_PrintPreallocated failed (query too long?)");
        cJSON_Delete(root);
        return false;
    }
    cJSON_Delete(root);
    int blen = (int)strlen(body);

    esp_http_client_config_t cfg = {
        .url               = NTFY_URL_PUB,
        .method            = HTTP_METHOD_POST,
        .event_handler     = http_event,
        .timeout_ms        = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client)
    {
        ESP_LOGE(TAG, "publish: client_init failed");
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, blen);

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status < 200 || status >= 300)
    {
        ESP_LOGE(TAG, "publish failed: err=%s status=%d",
                 esp_err_to_name(err), status);
        return false;
    }

    ESP_LOGI(TAG, "publish OK id=%lu query=\"%s\"",
             (unsigned long)req_id, query);
    return true;
}

/* Look through s_respBuf (JSON-lines from ntfy poll) for a 'message' that
 * parses as {"id":<req_id>,"videoId":"..."}.  On match, copy videoId to
 * out and return true. */
static bool scan_response_for_id(uint32_t req_id,
                                 char    *out_video_id,
                                 size_t   max_len)
{
    char *line = s_respBuf;
    while (line && *line)
    {
        char *next = strchr(line, '\n');
        if (next) { *next = '\0'; }

        if (line[0] == '{')                           /* one ntfy event */
        {
            cJSON *evt = cJSON_Parse(line);
            if (evt)
            {
                cJSON *jmsg = cJSON_GetObjectItem(evt, "message");
                if (jmsg && cJSON_IsString(jmsg))
                {
                    cJSON *inner = cJSON_Parse(jmsg->valuestring);
                    if (inner)
                    {
                        cJSON *jid  = cJSON_GetObjectItem(inner, "id");
                        cJSON *jvid = cJSON_GetObjectItem(inner, "videoId");
                        if (jid && jvid &&
                            cJSON_IsNumber(jid) &&
                            (uint32_t)jid->valuedouble == req_id &&
                            cJSON_IsString(jvid) &&
                            jvid->valuestring[0] != '\0')
                        {
                            strncpy(out_video_id,
                                    jvid->valuestring,
                                    max_len - 1);
                            out_video_id[max_len - 1] = '\0';
                            cJSON_Delete(inner);
                            cJSON_Delete(evt);
                            return true;
                        }
                        cJSON_Delete(inner);
                    }
                }
                cJSON_Delete(evt);
            }
        }

        if (!next) { break; }
        line = next + 1;
    }
    return false;
}

/* One GET /res/json?since=20s&poll=1.  Populates s_respBuf, then scans
 * it for our req_id.  Returns true if videoId was found this poll.
 *
 * Uses ntfy.sh's relative duration syntax (`since=20s`) so the window
 * doesn't depend on NORA's wall clock — survives unsynced SNTP at boot. */
static bool poll_for_response(uint32_t req_id,
                              char    *out_video_id,
                              size_t   max_len)
{
    esp_http_client_config_t cfg = {
        .url               = NTFY_URL_POLL,
        .method            = HTTP_METHOD_GET,
        .event_handler     = http_event,
        .timeout_ms        = 3000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { return false; }

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200 || s_respLen <= 0)
    {
        return false;
    }
    return scan_response_for_id(req_id, out_video_id, max_len);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool ntfy_search(const char *query,
                 char       *out_video_id,
                 size_t      max_len,
                 uint32_t    timeout_ms)
{
    if (!query || !out_video_id || max_len < 12)
    {
        ESP_LOGE(TAG, "search: bad args");
        return false;
    }
    out_video_id[0] = '\0';

    uint32_t req_id = esp_random();

    if (!publish_request(req_id, query)) { return false; }

    const TickType_t start    = xTaskGetTickCount();
    const TickType_t deadline = start + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline)
    {
        vTaskDelay(pdMS_TO_TICKS(NTFY_POLL_INTERVAL_MS));
        if (poll_for_response(req_id, out_video_id, max_len))
        {
            ESP_LOGI(TAG, "got response id=%lu videoId=%s",
                     (unsigned long)req_id, out_video_id);
            return true;
        }
    }

    ESP_LOGW(TAG, "timeout waiting for id=%lu", (unsigned long)req_id);
    return false;
}
