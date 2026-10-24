/**
 * PBAP Client: fetch the connected phone's contacts into the whitelist.
 * Only used when HFP Client is running and the whitelist is enabled.
 */

#include "pbap_sync.h"

#include "bt_whitelist.h"
#include "config_manager.h"
#include "hfp_client_mgr.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "sdkconfig.h"

static const char *TAG = "pbap";

#if CONFIG_BT_PBAC_ENABLED

#include "esp_pbac_api.h"

#define PBAP_BUF_MAX      (96 * 1024)
#define PBAP_START_DELAY_MS  2000

static TimerHandle_t s_start_timer;
static esp_pbac_conn_hdl_t s_handle;
static bool s_busy;
static bool s_call_active;
static char *s_buf;
static size_t s_used;

static void free_buf(void)
{
    if (s_buf) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_used = 0;
}

static void request_phone_book(void)
{
    if (s_handle == ESP_PBAC_INVALID_HANDLE) {
        return;
    }
    free_buf();
    s_buf = heap_caps_malloc(PBAP_BUF_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL) {
        s_buf = malloc(PBAP_BUF_MAX);
    }
    if (s_buf == NULL) {
        ESP_LOGE(TAG, "No memory for phone book");
        esp_pbac_disconnect(s_handle);
        return;
    }

    /* VERSION + FN + N + TEL; skip PHOTO and the rest. */
    esp_pbac_pull_phone_book_app_param_t app = {0};
    app.include_property_selector = 1;
    app.property_selector = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 7);

    esp_err_t err = esp_pbac_pull_phone_book(s_handle, "telecom/pb.vcf", &app);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pull_phone_book: %s", esp_err_to_name(err));
        esp_pbac_disconnect(s_handle);
    }
}

static void pbac_cb(esp_pbac_event_t event, esp_pbac_param_t *param)
{
    switch (event) {
    case ESP_PBAC_CONNECTION_STATE_EVT:
        s_handle = param->conn_stat.handle;
        if (param->conn_stat.connected) {
            ESP_LOGI(TAG, "PBAP connected, pulling phone book");
            request_phone_book();
        } else {
            ESP_LOGI(TAG, "PBAP disconnected, reason=%d", (int)param->conn_stat.reason);
            s_handle = ESP_PBAC_INVALID_HANDLE;
            s_busy = false;
            free_buf();
        }
        break;

    case ESP_PBAC_PULL_PHONE_BOOK_RESPONSE_EVT: {
        const esp_pbac_status_t st = param->pull_phone_book_rsp.result;
        if (st != ESP_PBAC_SUCCESS) {
            ESP_LOGW(TAG, "Phone book pull failed: 0x%x (user denied?)", (unsigned)st);
            if (s_handle) {
                esp_pbac_disconnect(s_handle);
            }
            break;
        }
        /* Size-only probe (max_list_count=0): pull the real object next. */
        if (param->pull_phone_book_rsp.include_phone_book_size &&
            param->pull_phone_book_rsp.data_len == 0 &&
            !param->pull_phone_book_rsp.final) {
            break;
        }
        uint16_t n = param->pull_phone_book_rsp.data_len;
        const uint8_t *d = param->pull_phone_book_rsp.data;
        if (n > 0 && d && s_buf) {
            if (s_used + n > PBAP_BUF_MAX) {
                n = (uint16_t)(PBAP_BUF_MAX - s_used);
            }
            if (n > 0) {
                memcpy(s_buf + s_used, d, n);
                s_used += n;
            }
        }
        if (param->pull_phone_book_rsp.final) {
            if (s_used > 0) {
                bt_whitelist_import_vcf(s_buf, s_used);
            } else {
                ESP_LOGW(TAG, "Empty phone book");
            }
            free_buf();
            if (s_handle) {
                esp_pbac_disconnect(s_handle);
            }
        }
        break;
    }

    default:
        break;
    }
}

static void start_timer_cb(TimerHandle_t t)
{
    (void)t;
    if (s_call_active) {
        s_busy = false;
        return;
    }
    const system_config_t *cfg = config_get();
    if (!cfg || !cfg->bt_whitelist || cfg->bt_mode != BT_MODE_HFP) {
        s_busy = false;
        return;
    }
    uint8_t bda[6];
    if (!hfp_get_peer_bda(bda)) {
        ESP_LOGW(TAG, "No HFP peer for PBAP");
        s_busy = false;
        return;
    }
    ESP_LOGI(TAG, "Connecting PBAP (phone will ask to share contacts)");
    esp_err_t err = esp_pbac_connect(bda);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pbac_connect: %s", esp_err_to_name(err));
        s_busy = false;
    }
}

esp_err_t pbap_sync_init(void)
{
    s_start_timer = xTimerCreate("pbap", pdMS_TO_TICKS(PBAP_START_DELAY_MS),
                                 pdFALSE, NULL, start_timer_cb);
    esp_err_t err = esp_pbac_register_callback(pbac_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register callback: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_pbac_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "pbac_init: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "PBAP client ready");
    return ESP_OK;
}

void pbap_sync_on_hfp_connected(void)
{
    const system_config_t *cfg = config_get();
    if (!cfg || !cfg->bt_whitelist || cfg->bt_mode != BT_MODE_HFP) {
        return;
    }
    if (s_call_active) {
        ESP_LOGI(TAG, "PBAP deferred until call ends");
        return;
    }
    if (s_busy) {
        return;
    }
    s_busy = true;
    if (s_start_timer) {
        xTimerStop(s_start_timer, 0);
        xTimerStart(s_start_timer, 0);
    }
}

void pbap_sync_set_call_active(bool active)
{
    s_call_active = active;
    if (!active) {
        return;
    }
    if (s_start_timer) {
        xTimerStop(s_start_timer, 0);
    }
    if (s_handle != ESP_PBAC_INVALID_HANDLE) {
        ESP_LOGI(TAG, "Pausing PBAP for call");
        esp_pbac_disconnect(s_handle);
        s_handle = ESP_PBAC_INVALID_HANDLE;
    }
    s_busy = false;
    free_buf();
}

void pbap_sync_on_hfp_disconnected(void)
{
    if (s_start_timer) {
        xTimerStop(s_start_timer, 0);
    }
    if (s_handle != ESP_PBAC_INVALID_HANDLE) {
        esp_pbac_disconnect(s_handle);
        s_handle = ESP_PBAC_INVALID_HANDLE;
    }
    s_busy = false;
    free_buf();
}

#else /* !CONFIG_BT_PBAC_ENABLED */

esp_err_t pbap_sync_init(void)
{
    ESP_LOGW(TAG, "CONFIG_BT_PBAC_ENABLED is off — contact sync disabled");
    return ESP_OK;
}

void pbap_sync_on_hfp_connected(void) {}
void pbap_sync_on_hfp_disconnected(void) {}
void pbap_sync_set_call_active(bool active) { (void)active; }

#endif
