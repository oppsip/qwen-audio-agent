/**
 * WiFi + Classic BT coexistence helpers for voice.
 *
 * ESP32 shares one 2.4 GHz radio. Ethernet leaves SCO alone; WiFi STA
 * must yield airtime to SCO/eSCO during a call or audio stutters.
 */

#include "audio_coex.h"
#include "config_manager.h"
#include "ethernet_manager.h"
#include "wifi_manager.h"

#include "esp_log.h"
#include "sdkconfig.h"
#include <stdbool.h>

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
#include "esp_coexist.h"
#endif

static const char *TAG = "audio_coex";

static volatile bool s_media_active;
static volatile bool s_http_busy;

/*
 * Bluedroid HFP AG has no public WBS toggle in ESP-IDF 6. This global is
 * consulted at AT+BAC and is not static, so we can prefer CVSD on WiFi.
 */
#if CONFIG_BT_HFP_AG_ENABLE
extern bool btc_conf_hf_force_wbs;
#endif

void audio_coex_hf_client_prefer_msbc(bool enable);

bool audio_path_on_wifi(void)
{
    return wifi_manager_is_connected() && !ethernet_manager_is_connected();
}

void audio_coex_apply_wbs_preference(void)
{
    bool nbs = audio_path_prefer_narrowband();
#if CONFIG_BT_HFP_AG_ENABLE
    btc_conf_hf_force_wbs = nbs ? 0 : 1;
    ESP_LOGI(TAG, "HFP AG WBS preferred=%d (wifi_bearer=%d)",
             (int)!nbs, (int)audio_path_on_wifi());
#endif
    /* HF role: AT+BAC=1 (CVSD only) on WiFi, AT+BAC=1,2 on Ethernet. */
    audio_coex_hf_client_prefer_msbc(!nbs);
}

void audio_coex_set_idle(void)
{
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    if (!s_media_active && !s_http_busy) {
        esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
    }
#else
    ESP_LOGW(TAG, "Software coexistence is off — rebuild with "
             "CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y");
#endif
    audio_coex_apply_wbs_preference();
}

void audio_coex_set_media(bool active)
{
    s_media_active = active;
    const system_config_t *cfg = config_get();
    bool a2dp = cfg && cfg->bt_mode == BT_MODE_A2DP;

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    if (active) {
        esp_coex_preference_set(ESP_COEX_PREFER_BT);
        if (a2dp) {
            esp_coex_status_bit_set(ESP_COEX_ST_TYPE_BT,
                                    ESP_COEX_BT_ST_A2DP_STREAMING);
        }
        ESP_LOGI(TAG, "Coex PREFER_BT (media, wifi=%d a2dp=%d)",
                 (int)audio_path_on_wifi(), (int)a2dp);
    } else {
        if (a2dp) {
            esp_coex_status_bit_clear(ESP_COEX_ST_TYPE_BT,
                                      ESP_COEX_BT_ST_A2DP_STREAMING);
        }
        if (s_http_busy) {
            esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
        } else {
            esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
        }
        ESP_LOGI(TAG, "Coex restored after media");
    }
#else
    (void)a2dp;
    ESP_LOGW(TAG, "Software coexistence is off — SCO+WiFi will stutter");
#endif
    audio_coex_apply_wbs_preference();
}

bool audio_coex_media_active(void)
{
    return s_media_active;
}

void audio_coex_set_http(bool busy)
{
    s_http_busy = busy;
    if (s_media_active) {
        ESP_LOGI(TAG, "HTTP coex ignored, media holds PREFER_BT");
        return;
    }
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE
    esp_coex_preference_set(busy ? ESP_COEX_PREFER_WIFI : ESP_COEX_PREFER_BALANCE);
#endif
}

uint16_t audio_path_sco_hwm(sco_codec_t codec)
{
    /* PCM bytes/ms: mSBC 16 kHz × 2 = 32, CVSD 8 kHz × 2 = 16 */
    unsigned bpm = (codec == SCO_CODEC_MSBC) ? 32u : 16u;
    /* Headroom above the bridge's playout target (see sco_target_ms) so the
     * trim only ever fires on a genuine pump stall, not on normal depth. */
    unsigned ms = audio_path_on_wifi() ? 140u : 80u;
    unsigned bytes = bpm * ms;
    if (bytes > 3840u) {
        bytes = 3840u; /* keep below SCO_OUT_RING_SIZE-1 */
    }
    return (uint16_t)bytes;
}

bool audio_path_prefer_narrowband(void)
{
    return audio_path_on_wifi();
}
