/*
 * NORA BLE Bridge
 * iPhone → BLE (Nordic UART Service) → ESP32-S3 → UART → STM32
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/* ── Configuration ──────────────────────────────────────────────────────── */
#define TAG            "NORA"
#define DEVICE_NAME    "NORA-Bridge"

/* UART — verify TX/RX pin numbers against your EVK-NORA-W106 schematic */
#define UART_PORT      UART_NUM_1
#define UART_TX_PIN    17
#define UART_RX_PIN    18
#define UART_BAUD      921600
#define UART_BUF_SIZE  1024

/* ── Nordic UART Service UUIDs (little-endian byte order for NimBLE) ─────
 * Service  : 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX char  : 6E400002-B5A3-F393-E0A9-E50E24DCCA9E  (Write from phone)
 * ─────────────────────────────────────────────────────────────────────── */
static const ble_uuid128_t nus_svc_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
                     0x93,0xf3,0xa3,0xb5,0x01,0x00,0x40,0x6e);

static const ble_uuid128_t nus_rx_uuid =
    BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,
                     0x93,0xf3,0xa3,0xb5,0x02,0x00,0x40,0x6e);

/* Forward declaration */
static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* ── GATT: RX characteristic callback ───────────────────────────────────── */
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t  buf[512];
    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }

    uint16_t out_len;
    ble_hs_mbuf_to_flat(ctxt->om, buf, len, &out_len);

    uart_write_bytes(UART_PORT, buf, out_len);

    ESP_LOGD(TAG, "BLE→UART %u bytes", out_len);
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

    /* Advertising data: flags + device name */
    struct ble_hs_adv_fields fields = {
        .flags                 = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name                  = (uint8_t *)DEVICE_NAME,
        .name_len              = strlen(DEVICE_NAME),
        .name_is_complete      = 1,
        .tx_pwr_lvl            = BLE_HS_ADV_TX_PWR_LVL_AUTO,
        .tx_pwr_lvl_is_present = 1,
    };

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields error: %d", rc);
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
            ESP_LOGI(TAG, "Connected  handle=%d", event->connect.conn_handle);
        } else {
            ESP_LOGI(TAG, "Connection failed, resuming advertising");
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
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

    uart_init();

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

    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "NORA BLE Bridge started");
}
