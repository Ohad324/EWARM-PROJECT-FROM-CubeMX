/*
 * NORA Wi-Fi + BLE Translate Bridge
 * STM32 → UART → ESP32-S3 → (Wi-Fi / HTTP translation) → UART → STM32
 * iPhone → BLE (Nordic UART Service) → ESP32-S3 → UART → STM32  (kept intact)
 *
 * Music feature:
 *   BLE "PLAY:<query>"  → YouTube search → thumbnail download → TRACK:/THUMB: to STM32
 */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "music_task.h"
#include "cloud_upload.h"
#include "command_router.h"
#include "esp_heap_caps.h"  /* heap_caps_malloc / heap_caps_free for PSRAM */

/* Maximum WAV file size NORA will buffer from STM32 (4 MB = ~64 s at 31250 Hz) */
#define AUDIO_FILE_MAX_BYTES  (4u * 1024u * 1024u)

/* ── Configuration ──────────────────────────────────────────────────────── */
#define TAG              "NORA"
#define MUSIC_TAG        "MUSIC"
#define DEVICE_NAME      "NORA-Bridge"

/* UART — verify TX/RX pin numbers against your EVK-NORA-W106 schematic     */
#define UART_PORT        UART_NUM_1
#define UART_TX_PIN      17
#define UART_RX_PIN      18
#define UART_BAUD        921600
#define UART_BUF_SIZE    1024

/* Command parser */
#define CMD_BUF_SIZE     256

/* BLE translate queue */
#define BLE_WORD_MAX     256    /* max chars from phone */
#define BLE_DEFAULT_LANGPAIR "en|he"

/* Wi-Fi credentials — change before flashing */
#define WIFI_SSID        "Sightsys_SEC24"
#define WIFI_PASSWORD    "0542584033"
#define WIFI_MAX_RETRY   5

/* ── Google Translate response buffer ───────────────────────────────────── */
#define HTTP_RESP_BUF_SIZE 4096

/* ── Nordic UART Service UUIDs (little-endian byte order for NimBLE) ─────
 * Service  : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX char  : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (Write from phone)
 * TX char  : 6E400003-B5A3-F393-E0A9-E50E24DCCA9E  (Notify to phone/PC)
 * ─────────────────────────────────────────────────────────────────────── */
static const ble_uuid128_t nus_svc_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
                     0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);

static const ble_uuid128_t nus_rx_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
                     0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);

static const ble_uuid128_t nus_tx_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
                     0x93,0xf3,0xa3,0xb5,0x03,0x00,0x40,0x6e);

/* ── Queues ──────────────────────────────────────────────────────────────── */
/* BLE → translate queue (populated by nus_rx_access, consumed by ble_translate_task) */
static QueueHandle_t s_ble_queue;

/* ── UART concurrency ────────────────────────────────────────────────────── *
 * s_uart_mutex : protects all uart_write_bytes() calls from concurrent     *
 *                tasks (translate vs music).                                 *
 * ─────────────────────────────────────────────────────────────────────────*/
static SemaphoreHandle_t  s_uart_mutex;
static uint16_t           s_nus_tx_handle = 0;                        /* TX char value handle   */
static uint16_t           s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;  /* current BLE connection */

/* Forward declaration */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* ── GATT: TX characteristic callback (notify only) ─────────────────────── */
static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

/* ── BLE notify send ────────────────────────────────────────────────────── *
 * Sends a UTF-8 string to the connected central via NUS TX notification.   *
 * No-op if not connected or characteristic not yet registered.             *
 * ─────────────────────────────────────────────────────────────────────── */
void ble_notify_send(const char *data)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_nus_tx_handle == 0) {
        ESP_LOGW(TAG, "BLE notify: no active connection");
        return;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, strlen(data));
    if (!om) {
        ESP_LOGE(TAG, "BLE notify: mbuf alloc failed");
        return;
    }

    int rc = ble_gatts_notify_custom(s_conn_handle, s_nus_tx_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE notify failed: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "BLE notify sent: %s", data);
    }
}

/* ── UART write helper (mutex-protected) ─────────────────────────────────── */
static void uart_write_safe(const void *data, int len)
{
    xSemaphoreTake(s_uart_mutex, portMAX_DELAY);
    uart_write_bytes(UART_PORT, data, len);
    xSemaphoreGive(s_uart_mutex);
}

/* ── GATT: RX characteristic callback ───────────────────────────────────── */
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    char     buf[BLE_WORD_MAX];
    if (len > sizeof(buf) - 1) {
        len = sizeof(buf) - 1;
    }

    uint16_t out_len;
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out_len);

    /* Null-terminate and strip trailing CR/LF */
    buf[out_len] = '\0';
    while (out_len > 0 && (buf[out_len - 1] == '\n' || buf[out_len - 1] == '\r')) {
        buf[--out_len] = '\0';
    }

    if (out_len > 0) {
        if (strncasecmp(buf, "PLAY:", 5) == 0) {
            /* Music request — forward to music_task module */
            music_request(buf + 5);
        } else {
            /* Translation request — existing pipeline unchanged */
            xQueueSend(s_ble_queue, buf, 0);          /* non-blocking; drop if full */
            ESP_LOGD(TAG, "BLE word queued: '%s'", buf);
        }
    }
    return 0;
}

/* ── GATT service table ─────────────────────────────────────────────────── */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &nus_rx_uuid.u,
                .access_cb = nus_rx_access,
                .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid       = &nus_tx_uuid.u,
                .access_cb  = nus_tx_access,
                .val_handle = &s_nus_tx_handle,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 }, /* terminator */
        },
    },
    { 0 }, /* terminator */
};

/* ── Advertising ────────────────────────────────────────────────────────── */
static void start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };

    /* Advertising packet: flags + name only (fits in 31 bytes) */
    struct ble_hs_adv_fields fields = {
        .flags            = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name             = (uint8_t *)DEVICE_NAME,
        .name_len         = strlen(DEVICE_NAME),
        .name_is_complete = 1,
    };

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields error: %d", rc);
        return;
    }

    /* Scan response: NUS UUID128 so nRF UART / Bluefruit apps auto-identify us */
    static const ble_uuid128_t uuids128[] = { nus_svc_uuid };
    struct ble_hs_adv_fields rsp_fields = {
        .uuids128             = uuids128,
        .num_uuids128         = 1,
        .uuids128_is_complete = 1,
    };

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields error: %d", rc);
        return;
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start error: %d", rc);
    } else {
        ESP_LOGI(TAG, "Advertising as \"%s\"", DEVICE_NAME);
    }
}

/* ── GAP event handler ──────────────────────────────────────────────────── */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected  handle=%d", s_conn_handle);
        } else {
            ESP_LOGI(TAG, "Connection failed, resuming advertising");
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGI(TAG, "Disconnected reason=%d, resuming advertising",
                 event->disconnect.reason);
        start_advertising();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU updated: %d", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ── NimBLE host sync callback ──────────────────────────────────────────── */
static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "BLE host reset, reason: %d", reason);
}

/* ── NimBLE host task ───────────────────────────────────────────────────── */
static void ble_host_task(void *param)
{
    nimble_port_run();              /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ── UART init ──────────────────────────────────────────────────────────── */
static void uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_PIN, UART_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART%d ready  TX=GPIO%d  RX=GPIO%d  %dbaud",
             UART_PORT, UART_TX_PIN, UART_RX_PIN, UART_BAUD);
}

/* ── Wi-Fi ──────────────────────────────────────────────────────────────── */
#define WIFI_CONNECTED_BIT  BIT0
static EventGroupHandle_t   s_wifi_event_group;
static int                  s_wifi_retry = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_wifi_retry < WIFI_MAX_RETRY) {
            s_wifi_retry++;
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", s_wifi_retry, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi: giving up after %d retries", WIFI_MAX_RETRY);
            uart_write_bytes(UART_PORT, "ERROR:wifi failed\n", 18);
        }

    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        uart_write_bytes(UART_PORT, "READY\n", 6);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid      = WIFI_SSID,
            .password  = WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi init done, connecting to \"%s\"...", WIFI_SSID);
}

/* ── URL encoder ────────────────────────────────────────────────────────── *
 * Percent-encodes every byte that is not an RFC-3986 unreserved character.
 * ─────────────────────────────────────────────────────────────────────── */
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

/* ── Google Translate (unofficial) ─────────────────────────────────────── *
 * Endpoint: GET https://translate.googleapis.com/translate_a/single
 *           ?client=gtx&sl=<src>&tl=<tgt>&dt=t&q=<word>
 * langpair : "en|he", "he|en", "en|fr", etc.  (src|tgt as ISO 639-1)
 * word     : UTF-8 text to translate
 * out_buf  : receives the translated string on success
 * Returns true on success, false on any error.
 * ─────────────────────────────────────────────────────────────────────── */
static bool http_translate(const char *langpair, const char *word,
                            char *out_buf, size_t out_len)
{
    /* Split "en|he" into src="en" and tgt="he" */
    char src[8] = "en";
    char tgt[8] = "he";
    const char *pipe = strchr(langpair, '|');
    if (pipe) {
        size_t src_len = (size_t)(pipe - langpair);
        if (src_len > 0 && src_len < sizeof(src)) {
            memcpy(src, langpair, src_len);
            src[src_len] = '\0';
        }
        strncpy(tgt, pipe + 1, sizeof(tgt) - 1);
        tgt[sizeof(tgt) - 1] = '\0';
    }

    char encoded[768];
    url_encode(word, encoded, sizeof(encoded));

    char url[1024];
    snprintf(url, sizeof(url),
             "https://translate.googleapis.com/translate_a/single"
             "?client=gtx&sl=%s&tl=%s&dt=t&q=%s",
             src, tgt, encoded);

    char *resp_buf = calloc(1, HTTP_RESP_BUF_SIZE);
    if (!resp_buf) return false;

    esp_http_client_config_t config = {
        .url               = url,
        .timeout_ms        = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { free(resp_buf); return false; }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(resp_buf);
        return false;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "Google Translate HTTP status=%d content_len=%d", status, content_len);

    int read = esp_http_client_read(client, resp_buf, HTTP_RESP_BUF_SIZE - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read <= 0) {
        ESP_LOGE(TAG, "HTTP read failed (read=%d)", read);
        free(resp_buf);
        return false;
    }
    resp_buf[read] = '\0';
    ESP_LOGI(TAG, "Google Translate response: %.200s", resp_buf);

    /* Parse: [[[" translation","original",...]],...] → [0][0][0] */
    cJSON *root = cJSON_Parse(resp_buf);
    free(resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "JSON parse failed");
        return false;
    }

    bool ok = false;
    cJSON *arr0 = cJSON_GetArrayItem(root, 0);
    cJSON *arr00 = arr0 ? cJSON_GetArrayItem(arr0, 0) : NULL;
    cJSON *translation = arr00 ? cJSON_GetArrayItem(arr00, 0) : NULL;

    if (!translation || !cJSON_IsString(translation) || !translation->valuestring) {
        ESP_LOGE(TAG, "JSON: could not extract [0][0][0]");
    } else {
        ESP_LOGI(TAG, "Google Translate [%s→%s] '%s' → '%s'",
                 src, tgt, word, translation->valuestring);
        strncpy(out_buf, translation->valuestring, out_len - 1);
        out_buf[out_len - 1] = '\0';
        ok = true;
    }
    cJSON_Delete(root);
    return ok;
}


/* ── BLE translate task ─────────────────────────────────────────────────── *
 * Dequeues words sent from the iPhone over BLE, calls the HTTP translation
 * API, and sends the result over UART to the STM32 / TouchGFX display.
 *
 * The phone may send either:
 *   "hello"         → translated with BLE_DEFAULT_LANGPAIR (en|he)
 *   "en|fr:hello"   → translated with the specified langpair
 * ─────────────────────────────────────────────────────────────────────── */
static void ble_translate_task(void *param)
{
    char word[BLE_WORD_MAX];
    ESP_LOGI(TAG, "BLE translate task started");

    while (1) {
        if (xQueueReceive(s_ble_queue, word, portMAX_DELAY) != pdTRUE) continue;

        /* Parse optional "langpair:word" prefix, e.g. "en|fr:hello" */
        char        langpair[32];
        const char *text  = word;
        const char *pipe  = strchr(word, '|');
        const char *colon = strchr(word, ':');

        if (pipe && colon && pipe < colon) {
            size_t lp_len = (size_t)(colon - word);
            if (lp_len > 0 && lp_len < sizeof(langpair)) {
                memcpy(langpair, word, lp_len);
                langpair[lp_len] = '\0';
                text = colon + 1;
            } else {
                strcpy(langpair, BLE_DEFAULT_LANGPAIR);
            }
        } else {
            strcpy(langpair, BLE_DEFAULT_LANGPAIR);
        }

        if (*text == '\0') continue;

        /* Wait up to 15 s for Wi-Fi */
        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                               WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(15000));
        if (!(bits & WIFI_CONNECTED_BIT)) {
            uart_write_safe("ERROR:wifi not ready\n", 21);
            ESP_LOGW(TAG, "BLE translate: Wi-Fi not connected");
            continue;
        }

        char translation[512] = {0};
        ESP_LOGI(TAG, "BLE translate [%s] '%s'", langpair, text);

        if (http_translate(langpair, text, translation, sizeof(translation))) {
            char reply[CMD_BUF_SIZE];
            int  len = snprintf(reply, sizeof(reply), "RESULT:%s\n", translation);
            uart_write_safe(reply, len);
            ESP_LOGI(TAG, "BLE→UART [%s] '%s' → '%s'", langpair, text, translation);
        } else {
            uart_write_safe("ERROR:translation failed\n", 25);
            ESP_LOGE(TAG, "BLE translation failed for '%s'", text);
        }
    }
}


/* ── UART command parser task ───────────────────────────────────────────── *
 * Reads line-terminated commands from STM32 over UART and replies.
 * Protocol:
 *   TRANSLATE:<langpair>:<word>\n  →  RESULT:<translation>\n
 *                                     ERROR:<reason>\n on failure
 *
 * Examples:
 *   TRANSLATE:en|he:hello\n   →  RESULT:שלום\n
 *   TRANSLATE:he|en:שלום\n    →  RESULT:hello\n
 *
 * Yields (10 ms delay) whenever music_task owns the UART for MP3 transfer,
 * preventing this task from consuming MP3_ACK / CHUNK_ACK bytes.
 * ─────────────────────────────────────────────────────────────────────── */
static void uart_cmd_task(void *param)
{
    uint8_t byte;
    char    line[CMD_BUF_SIZE];
    int     pos = 0;

    ESP_LOGI(TAG, "UART command task started");

    while (1) {
        int n = uart_read_bytes(UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (n <= 0) continue;

        if (byte == '\r') continue;          /* ignore CR */

        if (byte != '\n') {
            if (pos < CMD_BUF_SIZE - 1) {
                line[pos++] = (char)byte;
            }
            continue;
        }

        /* newline — null-terminate and dispatch */
        line[pos] = '\0';
        pos = 0;

        if (line[0] == '\0') continue;

        if (strncmp(line, "TRANSLATE:", 10) == 0) {
            const char *rest = line + 10;

            /* Split "en|he:hello" into langpair="en|he", word="hello" */
            const char *colon = strchr(rest, ':');
            if (!colon) {
                uart_write_safe("ERROR:bad format\n", 17);
                continue;
            }

            char langpair[32];
            size_t lp_len = (size_t)(colon - rest);
            if (lp_len == 0 || lp_len >= sizeof(langpair)) {
                uart_write_safe("ERROR:bad langpair\n", 19);
                continue;
            }
            memcpy(langpair, rest, lp_len);
            langpair[lp_len] = '\0';

            const char *word = colon + 1;
            if (*word == '\0') {
                uart_write_safe("ERROR:empty word\n", 17);
                continue;
            }

            /* Wait up to 15 s for Wi-Fi before attempting translation */
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT,
                                                   pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(15000));
            if (!(bits & WIFI_CONNECTED_BIT)) {
                uart_write_safe("ERROR:wifi not ready\n", 21);
                ESP_LOGW(TAG, "Translation requested but Wi-Fi not connected");
                continue;
            }

            char translation[512] = {0};
            ESP_LOGI(TAG, "Translating [%s] '%s'", langpair, word);

            if (http_translate(langpair, word, translation, sizeof(translation))) {
                char reply[CMD_BUF_SIZE];
                int  len = snprintf(reply, sizeof(reply), "RESULT:%s\n", translation);
                uart_write_safe(reply, len);
                ESP_LOGI(TAG, "TRANSLATE [%s] '%s' -> '%s'", langpair, word, translation);
            } else {
                uart_write_safe("ERROR:translation failed\n", 25);
                ESP_LOGE(TAG, "Translation failed for '%s'", word);
            }

        } else if (strncmp(line, "AUDIO:FILE:", 11) == 0) {
            /* ── Receive WAV file from STM32, upload to GCS, transcribe, route ──
             * Protocol: "AUDIO:FILE:REC_001.wav:65536\n" + <65536 raw bytes>
             * ──────────────────────────────────────────────────────────────── */
            const char *rest      = line + 11;          /* "REC_001.wav:65536" */
            const char *lastColon = strrchr(rest, ':'); /* points to ":65536"  */

            if (!lastColon)
            {
                uart_write_safe("UPLOAD:FAIL:bad_header\n", 23);
                ESP_LOGE(TAG, "AUDIO:FILE bad header: '%s'", line);
                continue;
            }

            /* Extract filename and byte count */
            char     filename[32];
            size_t   fnLen   = (size_t)(lastColon - rest);
            uint32_t fileSize = (uint32_t)atoi(lastColon + 1);

            if (fnLen == 0 || fnLen >= sizeof(filename) ||
                fileSize == 0 || fileSize > AUDIO_FILE_MAX_BYTES)
            {
                uart_write_safe("UPLOAD:FAIL:bad_params\n", 23);
                ESP_LOGE(TAG, "AUDIO:FILE bad params: fnLen=%zu size=%lu",
                         fnLen, (unsigned long)fileSize);
                continue;
            }
            memcpy(filename, rest, fnLen);
            filename[fnLen] = '\0';

            ESP_LOGI(TAG, "AUDIO:FILE '%s' %lu bytes — allocating PSRAM",
                     filename, (unsigned long)fileSize);

            /* Allocate from PSRAM (8 MB available on ESP32-S3) */
            uint8_t *wavBuf = heap_caps_malloc(fileSize, MALLOC_CAP_SPIRAM);
            if (!wavBuf)
            {
                uart_write_safe("UPLOAD:FAIL:no_mem\n", 19);
                ESP_LOGE(TAG, "PSRAM alloc failed for %lu bytes", (unsigned long)fileSize);
                continue;
            }

            /* Receive raw WAV bytes from STM32 in 1-KB chunks.
             * 5-second timeout per chunk: STM32 reads from SD so allow for
             * occasional SD read latency spikes. */
            uint32_t received = 0;
            bool     rxOk     = true;
            while (received < fileSize)
            {
                uint32_t toRead = fileSize - received;
                if (toRead > 1024u) toRead = 1024u;

                int got = uart_read_bytes(UART_PORT,
                                          wavBuf + received,
                                          (size_t)toRead,
                                          pdMS_TO_TICKS(5000));
                if (got <= 0)
                {
                    ESP_LOGE(TAG, "UART rx timeout at byte %lu/%lu",
                             (unsigned long)received, (unsigned long)fileSize);
                    rxOk = false;
                    break;
                }
                received += (uint32_t)got;
            }

            if (!rxOk || received != fileSize)
            {
                heap_caps_free(wavBuf);
                uart_write_safe("UPLOAD:FAIL:rx_error\n", 21);
                continue;
            }

            ESP_LOGI(TAG, "WAV received: %lu bytes — waiting for WiFi", (unsigned long)fileSize);

            /* Wait up to 5 s for WiFi before attempting upload */
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_CONNECTED_BIT,
                                                   pdFALSE, pdTRUE,
                                                   pdMS_TO_TICKS(5000));
            if (!(bits & WIFI_CONNECTED_BIT))
            {
                heap_caps_free(wavBuf);
                uart_write_safe("UPLOAD:FAIL:no_wifi\n", 20);
                ESP_LOGW(TAG, "Upload skipped: WiFi not connected");
                continue;
            }

            /* Upload → Transcribe → Route */
            if (CloudUpload_UploadWav(filename, wavBuf, (size_t)fileSize))
            {
                char transcript[CLOUD_TRANSCRIPT_MAX];
                if (CloudUpload_Transcribe(filename, transcript, sizeof(transcript)))
                {
                    CommandRouter_Route(transcript);
                }
            }

            heap_caps_free(wavBuf);

        } else {
            char reply[CMD_BUF_SIZE];
            int  len = snprintf(reply, sizeof(reply), "ERROR:unknown command\n");
            uart_write_safe(reply, len);
            ESP_LOGW(TAG, "Unknown cmd: '%s'", line);
        }
    }
}

/* ── Entry point ────────────────────────────────────────────────────────── */
void app_main(void)
{
    ESP_LOGI(TAG, "app_main() started");

    /* NVS required by BLE stack */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* UART mutex must exist before any task writes UART */
    s_uart_mutex = xSemaphoreCreateMutex();
    assert(s_uart_mutex);

    music_task_init(s_uart_mutex);
    CloudUpload_Init();
    CommandRouter_Init();

    uart_init();
    wifi_init();

    /* Wait for Wi-Fi to associate before starting BLE so the coexistence
     * arbiter does not starve the 4-way handshake.  If Wi-Fi never connects
     * we proceed anyway after the retry timeout so BLE still works.       */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, pdMS_TO_TICKS(35000));

    /* NimBLE init */
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* Register GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    assert(rc == 0);
    rc = ble_gatts_add_svcs(gatt_svcs);
    assert(rc == 0);

    /* Set device name visible in GAP */
    ble_svc_gap_device_name_set(DEVICE_NAME);

    /* BLE→translate queue (8 words deep) — must exist before BLE stack starts */
    s_ble_queue = xQueueCreate(8, BLE_WORD_MAX);
    assert(s_ble_queue);

    nimble_port_freertos_init(ble_host_task);

    /* Start tasks */
    xTaskCreate(ble_translate_task, "ble_xlat", 12288, NULL, 5, NULL);
    xTaskCreate(uart_cmd_task,      "uart_cmd", 12288, NULL, 5, NULL);

    ESP_LOGI(TAG, "NORA Wi-Fi+BLE Translate Bridge started");
}
