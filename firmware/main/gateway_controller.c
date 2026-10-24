/**
 * Gateway Controller — Bluetooth call state ↔ Qwen Audio Agent session
 *
 * Replaces the SIP INVITE/BYE machine. Bluetooth roles stay the same:
 *   HFP Client : phone incoming RING starts a Qwen session, then ATA
 *   HFP AG     : headset connect / ATD starts a Qwen session and opens SCO
 *   A2DP       : speaker connect starts a playback-only Qwen session
 */

#include "gateway_controller.h"
#include "qwen_client.h"
#include "audio_bridge.h"
#include "hfp_client_mgr.h"
#include "hfp_ag_mgr.h"
#include "a2dp_mgr.h"
#include "config_manager.h"
#include "bt_whitelist.h"
#include "pbap_sync.h"
#include "audio_coex.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "gateway";

static call_state_t s_state = CALL_STATE_IDLE;

static bool bt_mode_is_ag(void)
{
    const system_config_t *cfg = config_get();
    return cfg && cfg->bt_mode == BT_MODE_HFP_AG;
}

static bool bt_audio_ready(void)
{
    const system_config_t *cfg = config_get();
    if (!cfg) {
        return false;
    }
    if (cfg->bt_mode == BT_MODE_A2DP) {
        return a2dp_get_conn_state() == A2DP_STATE_CONNECTED;
    }
    if (cfg->bt_mode == BT_MODE_HFP_AG) {
        return hfp_ag_get_conn_state() >= HFP_STATE_SLC_CONNECTED;
    }
    return hfp_get_conn_state() >= HFP_STATE_SLC_CONNECTED;
}

static void bt_hangup(void)
{
    const system_config_t *cfg = config_get();
    if (!cfg) {
        return;
    }
    if (cfg->bt_mode == BT_MODE_HFP_AG) {
        hfp_ag_end_call();
    } else if (cfg->bt_mode == BT_MODE_HFP) {
        hfp_hangup();
    }
}

static void stop_session(void)
{
    audio_bridge_stop();
    qwen_client_interrupt();
    qwen_client_stop();
    audio_coex_set_media(false);
    pbap_sync_set_call_active(false);
    s_state = CALL_STATE_IDLE;
    ESP_LOGI(TAG, "Qwen session stopped");
}

static esp_err_t open_bt_audio(void)
{
    const system_config_t *cfg = config_get();
    if (!cfg) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cfg->bt_mode == BT_MODE_HFP_AG) {
        esp_err_t err = hfp_ag_notify_outgoing("Qwen");
        return err == ESP_OK ? hfp_ag_notify_call_active() : err;
    } else if (cfg->bt_mode == BT_MODE_HFP) {
        if (hfp_incoming_call_pending()) {
            return hfp_answer_call();
        }
        return hfp_call_is_active() ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

static void on_qwen_ready(int input_sample_rate)
{
    if (s_state != CALL_STATE_CONNECTING) {
        ESP_LOGW(TAG, "Ignoring Qwen voice.ready in call state %d", (int)s_state);
        return;
    }
    ESP_LOGI(TAG, "Qwen voice ready (%d Hz), starting media", input_sample_rate);

    const system_config_t *cfg = config_get();
    audio_bridge_config_t ab = {
        .sco_codec = bt_mode_is_ag() ? hfp_ag_get_sco_codec() : hfp_get_sco_codec(),
        .input_rate = input_sample_rate,
        .capture = !(cfg && cfg->bt_mode == BT_MODE_A2DP),
    };

    esp_err_t err = open_bt_audio();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Cannot open Bluetooth audio: %s", esp_err_to_name(err));
        stop_session();
        return;
    }

    if (audio_bridge_start(&ab) != ESP_OK) {
        ESP_LOGE(TAG, "audio_bridge_start failed");
        bt_hangup();
        stop_session();
        return;
    }

    audio_coex_set_media(true);
    pbap_sync_set_call_active(true);
    if (cfg && cfg->bt_mode == BT_MODE_A2DP) {
        qwen_client_set_input_muted(true);
    } else {
        qwen_client_set_input_muted(false);
    }
    s_state = CALL_STATE_ACTIVE;
    ESP_LOGI(TAG, "Call ACTIVE with Qwen Audio Agent");
}

static void on_qwen_audio(const int16_t *pcm, size_t samples, int sample_rate,
                          const char *response_id)
{
    if (s_state == CALL_STATE_ACTIVE) {
        audio_bridge_qwen_to_bt(pcm, samples, sample_rate, response_id);
    }
}

static void on_qwen_audio_done(const char *response_id)
{
    if (s_state == CALL_STATE_ACTIVE) {
        audio_bridge_on_audio_done(response_id);
    }
}

static void on_qwen_playback_clear(void)
{
    if (s_state == CALL_STATE_ACTIVE) {
        audio_bridge_clear_playback();
    }
}

static void on_qwen_disconnected(int close_code)
{
    ESP_LOGW(TAG, "Qwen disconnected (code=%d) state=%d", close_code, (int)s_state);
    if (s_state == CALL_STATE_IDLE) {
        return;
    }
    audio_bridge_stop();
    audio_coex_set_media(false);
    pbap_sync_set_call_active(false);
    bt_hangup();
    s_state = CALL_STATE_IDLE;
}

static void on_qwen_error(const char *message)
{
    ESP_LOGW(TAG, "Qwen error: %s", message ? message : "");
}

void gateway_try_start_session(const char *reason)
{
    if (s_state != CALL_STATE_IDLE) {
        ESP_LOGD(TAG, "Session start ignored (state=%d, reason=%s)",
                 (int)s_state, reason ? reason : "");
        return;
    }
    if (!bt_audio_ready()) {
        ESP_LOGW(TAG, "Cannot start Qwen: Bluetooth not ready (%s)",
                 reason ? reason : "");
        return;
    }

    const system_config_t *cfg = config_get();
    if (!cfg || cfg->qwen_host[0] == '\0') {
        ESP_LOGW(TAG, "Cannot start Qwen: host not configured");
        if (bt_mode_is_ag()) {
            hfp_ag_end_call();
        }
        return;
    }
    if (cfg->bt_mode == BT_MODE_HFP &&
        !hfp_incoming_call_pending() && !hfp_call_is_active()) {
        ESP_LOGW(TAG, "Cannot start Qwen: no phone call is pending or active (%s)",
                 reason ? reason : "");
        return;
    }

    ESP_LOGI(TAG, "Starting Qwen session (%s)", reason ? reason : "unspecified");
    s_state = CALL_STATE_CONNECTING;
    if (qwen_client_start() != ESP_OK) {
        ESP_LOGE(TAG, "qwen_client_start failed");
        s_state = CALL_STATE_IDLE;
        bt_hangup();
    }
}

esp_err_t gateway_init(void)
{
    s_state = CALL_STATE_IDLE;
    qwen_client_callbacks_t cbs = {
        .on_ready = on_qwen_ready,
        .on_audio_delta = on_qwen_audio,
        .on_audio_done = on_qwen_audio_done,
        .on_playback_clear = on_qwen_playback_clear,
        .on_disconnected = on_qwen_disconnected,
        .on_error = on_qwen_error,
    };
    esp_err_t err = qwen_client_init(&cbs);
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "Gateway controller initialized (Qwen Audio Agent)");
    return ESP_OK;
}

/* ---- HFP Client (phone) ---- */

static void gw_on_hfp_connection_state(hfp_conn_state_t state)
{
    ESP_LOGI(TAG, "HFP connection state: %d", state);
    if (state == HFP_STATE_SLC_CONNECTED) {
        pbap_sync_on_hfp_connected();
    } else if (state == HFP_STATE_DISCONNECTED) {
        pbap_sync_on_hfp_disconnected();
        hfp_hold_audio(false);
        if (s_state != CALL_STATE_IDLE) {
            stop_session();
        }
    }
}

static void gw_on_hfp_incoming_call(const char *number)
{
    const system_config_t *cfg = config_get();
    if (cfg && cfg->bt_mode == BT_MODE_HFP && cfg->bt_whitelist &&
        bt_whitelist_contains(number)) {
        if (s_state != CALL_STATE_IDLE) {
            return;
        }
        ESP_LOGI(TAG, "Whitelist CLIP %s — skip Qwen, keep audio on the phone",
                 number ? number : "");
        hfp_hold_audio(true);
        return;
    }
    hfp_hold_audio(false);
    gateway_try_start_session("phone RING");
}

static void gw_on_hfp_call_answered(void)
{
    ESP_LOGI(TAG, "HFP call answered");
}

static void gw_on_hfp_call_ended(void)
{
    if (s_state == CALL_STATE_IDLE) {
        return;
    }
    ESP_LOGI(TAG, "BT call ended, stopping Qwen");
    stop_session();
}

static void gw_on_hfp_outgoing_call_connected(void)
{
    ESP_LOGI(TAG, "BT outgoing connected");
}

static void gw_on_hfp_outgoing_call_failed(void)
{
    ESP_LOGW(TAG, "BT outgoing failed");
    if (s_state != CALL_STATE_IDLE) {
        stop_session();
    }
}

static void gw_on_hfp_audio_data(const uint8_t *data, uint32_t len)
{
    if (s_state == CALL_STATE_ACTIVE) {
        audio_bridge_bt_to_qwen(data, len);
    }
}

static void gw_on_hfp_audio_codec_changed(sco_codec_t codec)
{
    audio_bridge_set_sco_codec(codec);
}

const hfp_callbacks_t *gateway_get_hfp_callbacks(void)
{
    static const hfp_callbacks_t cbs = {
        .on_connection_state = gw_on_hfp_connection_state,
        .on_incoming_call = gw_on_hfp_incoming_call,
        .on_call_answered = gw_on_hfp_call_answered,
        .on_call_ended = gw_on_hfp_call_ended,
        .on_outgoing_call_connected = gw_on_hfp_outgoing_call_connected,
        .on_outgoing_call_failed = gw_on_hfp_outgoing_call_failed,
        .on_audio_data = gw_on_hfp_audio_data,
        .on_audio_codec_changed = gw_on_hfp_audio_codec_changed,
    };
    return &cbs;
}

/* ---- HFP AG (headset) ---- */

static void gw_on_ag_connection_state(hfp_conn_state_t state)
{
    ESP_LOGI(TAG, "HFP AG connection state: %d", state);
    if (state == HFP_STATE_SLC_CONNECTED) {
        const system_config_t *cfg = config_get();
        if (cfg && cfg->qwen_auto_session) {
            gateway_try_start_session("headset connected");
        }
    } else if (state == HFP_STATE_DISCONNECTED) {
        if (s_state != CALL_STATE_IDLE) {
            stop_session();
        }
    }
}

static void gw_on_ag_hf_answer(void)
{
    ESP_LOGI(TAG, "Headset ATA");
    if (s_state == CALL_STATE_IDLE) {
        gateway_try_start_session("headset ATA");
    }
}

static void gw_on_ag_hf_hangup(void)
{
    if (s_state == CALL_STATE_IDLE) {
        hfp_ag_end_call();
        return;
    }
    ESP_LOGI(TAG, "Headset hangup, stopping Qwen");
    stop_session();
    hfp_ag_end_call();
}

static void gw_on_ag_hf_dial(const char *number)
{
    ESP_LOGI(TAG, "Headset dial '%s'", number ? number : "");
    if (s_state != CALL_STATE_IDLE) {
        ESP_LOGW(TAG, "Headset dial while busy");
        return;
    }
    gateway_try_start_session("headset ATD");
}

static void gw_on_ag_audio_data(const uint8_t *data, uint32_t len)
{
    if (s_state == CALL_STATE_ACTIVE) {
        audio_bridge_bt_to_qwen(data, len);
    }
}

static void gw_on_ag_audio_codec_changed(sco_codec_t codec)
{
    audio_bridge_set_sco_codec(codec);
}

const hfp_ag_callbacks_t *gateway_get_hfp_ag_callbacks(void)
{
    static const hfp_ag_callbacks_t cbs = {
        .on_connection_state = gw_on_ag_connection_state,
        .on_hf_answer = gw_on_ag_hf_answer,
        .on_hf_hangup = gw_on_ag_hf_hangup,
        .on_hf_dial = gw_on_ag_hf_dial,
        .on_audio_data = gw_on_ag_audio_data,
        .on_audio_codec_changed = gw_on_ag_audio_codec_changed,
    };
    return &cbs;
}

/* ---- A2DP speaker ---- */

static void gw_on_a2dp_connection_state(a2dp_conn_state_t state)
{
    ESP_LOGI(TAG, "A2DP connection state: %d", state);
    if (state == A2DP_STATE_CONNECTED) {
        const system_config_t *cfg = config_get();
        if (cfg && cfg->qwen_auto_session) {
            gateway_try_start_session("A2DP speaker connected");
        }
    } else if (state == A2DP_STATE_DISCONNECTED) {
        if (s_state != CALL_STATE_IDLE) {
            stop_session();
        }
    }
}

const a2dp_callbacks_t *gateway_get_a2dp_callbacks(void)
{
    static const a2dp_callbacks_t cbs = {
        .on_connection_state = gw_on_a2dp_connection_state,
    };
    return &cbs;
}
