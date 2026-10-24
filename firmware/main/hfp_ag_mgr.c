/**
 * HFP Audio Gateway Manager
 *
 * ESP32 acts as HFP AG (like a phone). A Bluetooth headset (HF) pairs to it.
 * Call control: headset ATA/CHUP/ATD ↔ SIP. SCO PCM is bridged to RTP.
 *
 * Uses the ESP-IDF v6 Bluedroid HFP AG API (legacy Voice-over-HCI path).
 * HFP Client and AG can be compiled together but must not run at the same time.
 */

#include "hfp_ag_mgr.h"
#include "bt_scan.h"
#include "config_manager.h"
#include "audio_coex.h"
#include "sdkconfig.h"

#if !CONFIG_BT_HFP_AG_ENABLE
#error "HFP AG mode requires CONFIG_BT_HFP_AG_ENABLE=y"
#endif

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_ag_api.h"
#include "esp_hf_ag_legacy_api.h"
#include "esp_hf_defs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "freertos/portmacro.h"

static const char *TAG = "hfp_ag";

typedef enum {
    AG_CALL_IDLE = 0,
    AG_CALL_INCOMING,
    AG_CALL_DIALING,
    AG_CALL_ALERTING,
    AG_CALL_ACTIVE,
} ag_call_phase_t;

static volatile hfp_conn_state_t s_conn_state = HFP_STATE_DISCONNECTED;
static volatile sco_codec_t s_sco_codec = SCO_CODEC_CVSD;
static volatile ag_call_phase_t s_call_phase = AG_CALL_IDLE;

static hfp_ag_callbacks_t s_callbacks;
static esp_bd_addr_t s_peer_addr;
static bool s_peer_addr_valid = false;
static bool s_auto_reconnect = true;
static char s_call_number[32];
static char s_last_number[32];
static char s_target_name[32];
static bool s_scanning = false;

static TimerHandle_t s_reconnect_timer = NULL;
static TimerHandle_t s_connect_watchdog = NULL;
static TimerHandle_t s_ring_timer = NULL;

#define CONNECT_WATCHDOG_MS  20000
#define RECONNECT_DELAY_MS   8000
#define RING_INTERVAL_MS     3000

#define SCO_OUT_RING_SIZE  4096u
#define SCO_OUT_MASK       (SCO_OUT_RING_SIZE - 1u)
#define SCO_OUT_PREFILL    640u

static uint8_t *s_sco_out_buf = NULL;
static uint16_t s_sco_out_wr = 0;
static uint16_t s_sco_out_rd = 0;
static portMUX_TYPE s_sco_out_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_sco_cb_calls;
static uint32_t s_sco_pad_frames;
static uint32_t s_sco_trims;
static esp_timer_handle_t s_sco_tx_timer = NULL;
static volatile bool s_sco_tx_active = false;

/* Bytes Bluedroid asks for per SCO frame; learned from the first callback. */
static volatile uint32_t s_sco_frame_len = 0;

static void ag_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param);
static void ag_incoming_data_cb(const uint8_t *buf, uint32_t len);
static uint32_t ag_outgoing_data_cb(uint8_t *buf, uint32_t len);
static void start_reconnect(void);
static void reconnect_timer_cb(TimerHandle_t timer);
static void connect_watchdog_cb(TimerHandle_t timer);
static void ring_timer_cb(TimerHandle_t timer);
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
static void start_scan(void);
static void try_connect_headset(const esp_bd_addr_t bda);

static bool peer_ready(void)
{
    return s_peer_addr_valid && s_conn_state >= HFP_STATE_SLC_CONNECTED;
}

static void set_call_number(const char *number)
{
    if (number == NULL || number[0] == '\0') {
        s_call_number[0] = '\0';
        return;
    }
    strncpy(s_call_number, number, sizeof(s_call_number) - 1);
    s_call_number[sizeof(s_call_number) - 1] = '\0';
}

static void remember_last_number(const char *number)
{
    if (number == NULL || number[0] == '\0') {
        return;
    }
    strncpy(s_last_number, number, sizeof(s_last_number) - 1);
    s_last_number[sizeof(s_last_number) - 1] = '\0';
}

static void save_peer_to_nvs(const esp_bd_addr_t addr)
{
    nvs_handle_t h;
    if (nvs_open("hfp_ag", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "peer_addr", addr, sizeof(esp_bd_addr_t));
    nvs_commit(h);
    nvs_close(h);
}

static bool load_peer_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("hfp_ag", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = sizeof(esp_bd_addr_t);
    esp_err_t err = nvs_get_blob(h, "peer_addr", s_peer_addr, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(esp_bd_addr_t)) {
        s_peer_addr_valid = true;
        ESP_LOGI(TAG, "Loaded peer BD_ADDR: " ESP_BD_ADDR_STR,
                 ESP_BD_ADDR_HEX(s_peer_addr));
        return true;
    }
    return false;
}

static void sco_tx_pump_cb(void *arg)
{
    (void)arg;
    if (!s_sco_tx_active) {
        return;
    }
    esp_hf_ag_outgoing_data_ready();

    /* esp_timer coalesces pump ticks when WiFi keeps the timer task busy, and
     * skipped ticks leave audio stranded in the ring until the HWM trim cuts
     * it out (audible as a gap).  When the backlog approaches the HWM, ask for
     * one extra frame so the controller — which transmits on the air schedule,
     * immune to our software jitter — absorbs the arrears instead. */
    uint32_t frame = s_sco_frame_len;
    if (frame == 0 || s_sco_out_buf == NULL) {
        return;
    }
    uint16_t hwm = audio_path_sco_hwm(s_sco_codec);
    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t occupied = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);
    portEXIT_CRITICAL(&s_sco_out_mux);

    if (hwm > frame * 2u && occupied > hwm - frame * 2u) {
        esp_hf_ag_outgoing_data_ready();
    }
}

static void sco_tx_timer_start(void)
{
    s_sco_tx_active = true;
    if (s_sco_tx_timer != NULL) {
        return;
    }
    esp_timer_create_args_t ta = {
        .callback              = sco_tx_pump_cb,
        .name                  = "ag_sco_tx",
        .dispatch_method       = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&ta, &s_sco_tx_timer) == ESP_OK) {
        esp_timer_start_periodic(s_sco_tx_timer, 7500);
        ESP_LOGD(TAG, "SCO TX pump timer started");
    }
}

static void sco_tx_timer_stop(void)
{
    /* Must clear this before the stack processes SCO close: bta_ag_sco_event
     * loops while the PCM callback returns a full frame, even in CLOSING. */
    s_sco_tx_active = false;
    if (s_sco_tx_timer == NULL) {
        return;
    }
    esp_timer_stop(s_sco_tx_timer);
    esp_timer_delete(s_sco_tx_timer);
    s_sco_tx_timer = NULL;
}

static void sco_ring_reset_and_prefill(void)
{
    if (s_sco_out_buf == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_sco_out_mux);
    s_sco_out_wr = 0;
    s_sco_out_rd = 0;
    memset(s_sco_out_buf, 0, SCO_OUT_PREFILL);
    s_sco_out_wr = SCO_OUT_PREFILL;
    portEXIT_CRITICAL(&s_sco_out_mux);
}

static void stop_ring_timer(void)
{
    if (s_ring_timer) {
        xTimerStop(s_ring_timer, 0);
    }
}

/* phone_state_update without forcing SCO. RING/CLIP is sent when setup=INCOMING. */
static esp_err_t ag_update_phone_state(int num_active, int num_held,
                                       esp_hf_call_status_t call,
                                       esp_hf_call_setup_status_t setup)
{
    if (!peer_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    char *num = s_call_number;
    return esp_hf_ag_answer_call(s_peer_addr, num_active, num_held,
                                 call, setup, num, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
}

static void audio_connect(void)
{
    if (peer_ready()) {
        esp_hf_ag_audio_connect(s_peer_addr);
    }
}

static void fill_cind(esp_hf_call_status_t *call,
                      esp_hf_call_setup_status_t *setup)
{
    switch (s_call_phase) {
    case AG_CALL_INCOMING:
        *call = ESP_HF_CALL_STATUS_NO_CALLS;
        *setup = ESP_HF_CALL_SETUP_STATUS_INCOMING;
        break;
    case AG_CALL_DIALING:
        *call = ESP_HF_CALL_STATUS_NO_CALLS;
        *setup = ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING;
        break;
    case AG_CALL_ALERTING:
        *call = ESP_HF_CALL_STATUS_NO_CALLS;
        *setup = ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING;
        break;
    case AG_CALL_ACTIVE:
        *call = ESP_HF_CALL_STATUS_CALL_IN_PROGRESS;
        *setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
        break;
    default:
        *call = ESP_HF_CALL_STATUS_NO_CALLS;
        *setup = ESP_HF_CALL_SETUP_STATUS_IDLE;
        break;
    }
}

/* ---------- Target headset discovery (by configured name) ---------- */

static bool name_matches(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (s_target_name[0] == '\0') {
        return true;
    }
    return strcasecmp(name, s_target_name) == 0;
}

static void start_scan(void)
{
    if (s_conn_state != HFP_STATE_DISCONNECTED) {
        return;
    }
    s_scanning = true;
    ESP_LOGI(TAG, "Scanning for headset '%s'...",
             s_target_name[0] ? s_target_name : "<any>");
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                               bt_scan_inquiry_len(), 0);
}

static void try_connect_headset(const esp_bd_addr_t bda)
{
    memcpy(s_peer_addr, bda, sizeof(esp_bd_addr_t));
    s_peer_addr_valid = true;
    s_scanning = false;
    s_conn_state = HFP_STATE_CONNECTING;
    bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_CONNECTING);
    if (s_callbacks.on_connection_state) {
        s_callbacks.on_connection_state(HFP_STATE_CONNECTING);
    }
    if (s_connect_watchdog) {
        xTimerStart(s_connect_watchdog, 0);
    }
    ESP_LOGI(TAG, "Connecting HFP AG SLC to " ESP_BD_ADDR_STR,
             ESP_BD_ADDR_HEX(s_peer_addr));
    esp_err_t ret = esp_hf_ag_slc_connect(s_peer_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hf_ag_slc_connect failed: %s", esp_err_to_name(ret));
        s_conn_state = HFP_STATE_DISCONNECTED;
        bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_NONE);
        if (s_callbacks.on_connection_state) {
            s_callbacks.on_connection_state(HFP_STATE_DISCONNECTED);
        }
        start_reconnect();
    }
}

/* ---------- GAP: pairing / SSP so headsets can bond ---------- */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Auth success: %s", param->auth_cmpl.device_name);
            bt_scan_on_auth_cmpl(param->auth_cmpl.bda,
                                 (const char *)param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "Auth failed, status=%d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {0};
        if (param->pin_req.min_16_digit) {
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 16, pin_code);
        } else {
            pin_code[0] = '0';
            pin_code[1] = '0';
            pin_code[2] = '0';
            pin_code[3] = '0';
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin_code);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm, accepting");
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %" PRIu32, param->key_notif.passkey);
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "SSP passkey request — not supported, rejecting");
        break;

    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[64] = {0};
        bool found = bt_scan_on_disc_res(param, name, sizeof(name));
        if (!s_scanning || s_conn_state != HFP_STATE_DISCONNECTED) {
            break;
        }
        if (found) {
            ESP_LOGI(TAG, "Inquiry: %s", name);
        }
        if (found && name_matches(name)) {
            ESP_LOGI(TAG, "Found target headset: %s", name);
            esp_bt_gap_cancel_discovery();
            try_connect_headset(param->disc_res.bda);
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        bt_scan_on_disc_state(param);
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED &&
            s_scanning && s_conn_state == HFP_STATE_DISCONNECTED) {
            s_scanning = false;
            ESP_LOGW(TAG, "Headset '%s' not found, retry in %d ms",
                     s_target_name[0] ? s_target_name : "<any>", RECONNECT_DELAY_MS);
            start_reconnect();
        }
        break;

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            const char *rmt = (const char *)param->read_rmt_name.rmt_name;
            bt_scan_on_remote_name(param->read_rmt_name.bda, rmt);
            if (s_target_name[0]) {
                ESP_LOGI(TAG, "Remote name: %s", rmt);
                if (!name_matches(rmt) && s_conn_state >= HFP_STATE_CONNECTED) {
                    ESP_LOGW(TAG, "Rejecting '%s' (configured target is '%s')",
                             rmt, s_target_name);
                    esp_hf_ag_slc_disconnect(s_peer_addr);
                }
            }
        }
        break;

    default:
        break;
    }
}

esp_err_t hfp_ag_init(const char *device_name, const char *target_headset_name,
                      const hfp_ag_callbacks_t *callbacks)
{
    esp_err_t ret;

    if (device_name == NULL || callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(&s_callbacks, callbacks, sizeof(hfp_ag_callbacks_t));
    bt_scan_set_role(BT_MODE_HFP_AG);
    if (target_headset_name && target_headset_name[0]) {
        strncpy(s_target_name, target_headset_name, sizeof(s_target_name) - 1);
        s_target_name[sizeof(s_target_name) - 1] = '\0';
    } else {
        s_target_name[0] = '\0';
    }

    s_sco_out_buf = heap_caps_malloc(SCO_OUT_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_sco_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SCO ring buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_bt_sleep_disable();

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    config_clear_bt_bonds_if_pending();

    ret = esp_bt_gap_register_callback(gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
    }

    /* Advertise as a phone so headsets list this device while pairing. */
    esp_bt_cod_t cod = {0};
    cod.major = ESP_BT_COD_MAJOR_DEV_PHONE;
    cod.minor = 0x04; /* cellular / smartphone-class */
    cod.service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_TELEPHONY;
    esp_bt_gap_set_cod(cod, ESP_BT_INIT_COD);

    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, NULL);

    ret = esp_bt_gap_set_device_name(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hf_ag_register_callback(ag_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AG register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = esp_hf_ag_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AG init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    audio_coex_apply_wbs_preference();

    ret = esp_hf_ag_register_data_callback(ag_incoming_data_cb, ag_outgoing_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register data callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    s_reconnect_timer = xTimerCreate("ag_reconn", pdMS_TO_TICKS(RECONNECT_DELAY_MS),
                                     pdFALSE, NULL, reconnect_timer_cb);
    s_connect_watchdog = xTimerCreate("ag_wdog", pdMS_TO_TICKS(CONNECT_WATCHDOG_MS),
                                      pdFALSE, NULL, connect_watchdog_cb);
    s_ring_timer = xTimerCreate("ag_ring", pdMS_TO_TICKS(RING_INTERVAL_MS),
                                pdTRUE, NULL, ring_timer_cb);

    if (s_target_name[0]) {
        ESP_LOGI(TAG, "HFP AG initialized, device name: %s, target headset: %s",
                 device_name, s_target_name);
        start_scan();
    } else if (load_peer_from_nvs()) {
        ESP_LOGI(TAG, "HFP AG initialized, device name: %s (any headset)", device_name);
        start_reconnect();
    } else {
        ESP_LOGI(TAG, "HFP AG initialized, device name: %s (waiting for headset)",
                 device_name);
    }
    return ESP_OK;
}

hfp_conn_state_t hfp_ag_get_conn_state(void)
{
    return s_conn_state;
}

sco_codec_t hfp_ag_get_sco_codec(void)
{
    return s_sco_codec;
}

int hfp_ag_sco_out_level_ms(void)
{
    if (s_sco_out_buf == NULL || !s_sco_tx_active) {
        return -1;
    }
    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t occupied = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);
    portEXIT_CRITICAL(&s_sco_out_mux);

    /* PCM bytes/ms: mSBC 16 kHz × 2 = 32, CVSD 8 kHz × 2 = 16 */
    unsigned bpm = (s_sco_codec == SCO_CODEC_MSBC) ? 32u : 16u;
    return (int)(occupied / bpm);
}

void hfp_ag_sco_tx_stats(uint32_t *calls, uint32_t *pad, uint32_t *trims)
{
    if (calls) *calls = __atomic_load_n(&s_sco_cb_calls, __ATOMIC_RELAXED);
    if (pad) *pad = __atomic_load_n(&s_sco_pad_frames, __ATOMIC_RELAXED);
    if (trims) *trims = __atomic_load_n(&s_sco_trims, __ATOMIC_RELAXED);
}

void hfp_ag_set_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;
    if (!enable && s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    ESP_LOGI(TAG, "Auto-reconnect: %s", enable ? "enabled" : "disabled");
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_conn_state != HFP_STATE_DISCONNECTED) {
        return;
    }

    if (s_peer_addr_valid) {
        ESP_LOGI(TAG, "Auto-reconnect to " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(s_peer_addr));
        s_scanning = false;
        s_conn_state = HFP_STATE_CONNECTING;
        bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_CONNECTING);
        if (s_callbacks.on_connection_state) {
            s_callbacks.on_connection_state(HFP_STATE_CONNECTING);
        }
        if (s_connect_watchdog) {
            xTimerStart(s_connect_watchdog, 0);
        }
        esp_err_t ret = esp_hf_ag_slc_connect(s_peer_addr);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "esp_hf_ag_slc_connect failed: %s", esp_err_to_name(ret));
            s_conn_state = HFP_STATE_DISCONNECTED;
            bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_NONE);
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_DISCONNECTED);
            }
            if (s_target_name[0]) {
                start_scan();
            } else if (s_reconnect_timer) {
                xTimerStart(s_reconnect_timer, 0);
            }
        }
        return;
    }

    if (s_target_name[0]) {
        start_scan();
    }
}

static void start_reconnect(void)
{
    if (!s_auto_reconnect) {
        return;
    }
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (!s_peer_addr_valid && s_target_name[0] == '\0') {
        return;
    }
    if (s_reconnect_timer) {
        ESP_LOGI(TAG, "Scheduling reconnect in %d ms", RECONNECT_DELAY_MS);
        xTimerStart(s_reconnect_timer, 0);
    }
}

static void connect_watchdog_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "Connect watchdog — forcing recovery");
    if (s_peer_addr_valid) {
        esp_hf_ag_slc_disconnect(s_peer_addr);
    }
    s_conn_state = HFP_STATE_DISCONNECTED;
    if (s_peer_addr_valid) {
        bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_NONE);
    }
    if (s_callbacks.on_connection_state) {
        s_callbacks.on_connection_state(HFP_STATE_DISCONNECTED);
    }
    /* Named target: rescan rather than hammering a stale BD_ADDR. */
    if (s_target_name[0]) {
        s_peer_addr_valid = false;
        start_scan();
    } else {
        start_reconnect();
    }
}

static void ring_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    /* BTA already repeats RING/CLIP every 5s after IN_CALL_RES. */
    if (s_call_phase != AG_CALL_INCOMING || !peer_ready()) {
        stop_ring_timer();
    }
}

esp_err_t hfp_ag_notify_incoming(const char *number)
{
    if (!peer_ready()) {
        ESP_LOGE(TAG, "Cannot ring headset: SLC not connected");
        return ESP_ERR_INVALID_STATE;
    }

    set_call_number(number);
    if (s_call_number[0] == '\0') {
        strncpy(s_call_number, "0", sizeof(s_call_number) - 1);
    }
    s_call_phase = AG_CALL_INCOMING;
    /* Headset plays its own ringtone (out-of-band RING). */
    esp_hf_ag_bsir(s_peer_addr, ESP_HF_IN_BAND_RINGTONE_NOT_PROVIDED);
    /* BTA_AG_IN_CALL_RES → RING + CLIP, then RING every 5s until answered. */
    esp_err_t ret = ag_update_phone_state(0, 0,
                                          ESP_HF_CALL_STATUS_NO_CALLS,
                                          ESP_HF_CALL_SETUP_STATUS_INCOMING);
    if (s_ring_timer) {
        xTimerStart(s_ring_timer, 0);
    }
    ESP_LOGI(TAG, "Incoming RING to headset CLIP=%s (%s)",
             s_call_number, esp_err_to_name(ret));
    return ret;
}

esp_err_t hfp_ag_answer(void)
{
    if (!peer_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    stop_ring_timer();
    s_call_phase = AG_CALL_ACTIVE;
    esp_err_t ret = ag_update_phone_state(1, 0,
                                          ESP_HF_CALL_STATUS_CALL_IN_PROGRESS,
                                          ESP_HF_CALL_SETUP_STATUS_IDLE);
    audio_connect();
    return ret;
}

esp_err_t hfp_ag_reject(void)
{
    if (!peer_ready()) {
        s_call_phase = AG_CALL_IDLE;
        return ESP_ERR_INVALID_STATE;
    }
    stop_ring_timer();
    sco_tx_timer_stop();
    s_call_phase = AG_CALL_IDLE;
    char *num = s_call_number;
    /* reject_call already tears down SCO — do not call audio_disconnect(). */
    esp_err_t ret = esp_hf_ag_reject_call(s_peer_addr, 0, 0,
                                          ESP_HF_CALL_STATUS_NO_CALLS,
                                          ESP_HF_CALL_SETUP_STATUS_IDLE,
                                          num, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
    set_call_number(NULL);
    return ret;
}

esp_err_t hfp_ag_end_call(void)
{
    stop_ring_timer();
    sco_tx_timer_stop();
    s_call_phase = AG_CALL_IDLE;
    if (!s_peer_addr_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    char *num = s_call_number;
    esp_err_t ret = ESP_OK;
    if (s_conn_state >= HFP_STATE_SLC_CONNECTED) {
        /* end_call already disconnects SCO — a second audio_disconnect
         * races into BTA_AG_SCO_CLOSING_ST and can wedge BTU_TASK. */
        ret = esp_hf_ag_end_call(s_peer_addr, 0, 0,
                                 ESP_HF_CALL_STATUS_NO_CALLS,
                                 ESP_HF_CALL_SETUP_STATUS_IDLE,
                                 num, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
    }
    set_call_number(NULL);
    return ret;
}

esp_err_t hfp_ag_notify_outgoing(const char *number)
{
    if (!peer_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    set_call_number(number);
    remember_last_number(number);
    s_call_phase = AG_CALL_DIALING;
    char *num = s_call_number;
    return esp_hf_ag_out_call(s_peer_addr, 0, 0,
                              ESP_HF_CALL_STATUS_NO_CALLS,
                              ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING,
                              num, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
}

esp_err_t hfp_ag_notify_outgoing_alerting(void)
{
    if (!peer_ready() || s_call_phase != AG_CALL_DIALING) {
        return ESP_ERR_INVALID_STATE;
    }
    s_call_phase = AG_CALL_ALERTING;
    return ag_update_phone_state(0, 0,
                                 ESP_HF_CALL_STATUS_NO_CALLS,
                                 ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING);
}

esp_err_t hfp_ag_notify_call_active(void)
{
    if (!peer_ready()) {
        return ESP_ERR_INVALID_STATE;
    }
    stop_ring_timer();
    s_call_phase = AG_CALL_ACTIVE;
    esp_err_t ret = ag_update_phone_state(1, 0,
                                          ESP_HF_CALL_STATUS_CALL_IN_PROGRESS,
                                          ESP_HF_CALL_SETUP_STATUS_IDLE);
    audio_connect();
    return ret;
}

void hfp_ag_push_audio(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0 || s_sco_out_buf == NULL || !s_sco_tx_active) {
        return;
    }

    uint16_t hwm = audio_path_sco_hwm(s_sco_codec);
    uint16_t trim = 0;
    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t occupied = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);
    if (occupied > hwm) {
        trim = occupied - hwm;
        s_sco_out_rd = (uint16_t)((s_sco_out_rd + trim) & SCO_OUT_MASK);
        occupied = hwm;
    }
    uint16_t free_space = (SCO_OUT_RING_SIZE - 1u) - occupied;
    if (len > free_space) {
        len = free_space;
    }
    if (len > 0) {
        uint16_t to_end = SCO_OUT_RING_SIZE - s_sco_out_wr;
        if (len <= to_end) {
            memcpy(s_sco_out_buf + s_sco_out_wr, data, len);
        } else {
            memcpy(s_sco_out_buf + s_sco_out_wr, data, to_end);
            memcpy(s_sco_out_buf, data + to_end, len - to_end);
        }
        s_sco_out_wr = (uint16_t)(s_sco_out_wr + (uint16_t)len) & SCO_OUT_MASK;
    }
    portEXIT_CRITICAL(&s_sco_out_mux);

    if (trim > 0) {
        __atomic_add_fetch(&s_sco_trims, 1, __ATOMIC_RELAXED);
        static uint32_t s_trim_count = 0;
        s_trim_count++;
        if (s_trim_count <= 5 || (s_trim_count % 100) == 0) {
            ESP_LOGD(TAG, "SCO TX trim #%lu (%u bytes)",
                     (unsigned long)s_trim_count, (unsigned)trim);
        }
    }
}

/* ---------- AG event callback ---------- */

static void ag_cb(esp_hf_cb_event_t event, esp_hf_cb_param_t *param)
{
    switch (event) {
    case ESP_HF_CONNECTION_STATE_EVT: {
        esp_hf_connection_state_t state = param->conn_stat.state;
        ESP_LOGI(TAG, "Connection state: %d, peer: " ESP_BD_ADDR_STR,
                 state, ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));

        switch (state) {
        case ESP_HF_CONNECTION_STATE_CONNECTING:
            s_scanning = false;
            esp_bt_gap_cancel_discovery();
            if (s_reconnect_timer) {
                xTimerStop(s_reconnect_timer, 0);
            }
            if (s_connect_watchdog) {
                xTimerStart(s_connect_watchdog, 0);
            }
            s_conn_state = HFP_STATE_CONNECTING;
            bt_scan_set_link(param->conn_stat.remote_bda, BT_SCAN_LINK_CONNECTING);
            audio_coex_apply_wbs_preference();
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_CONNECTING);
            }
            break;

        case ESP_HF_CONNECTION_STATE_CONNECTED:
            s_scanning = false;
            esp_bt_gap_cancel_discovery();
            if (s_reconnect_timer) {
                xTimerStop(s_reconnect_timer, 0);
            }
            if (s_connect_watchdog) {
                xTimerStop(s_connect_watchdog, 0);
            }
            s_conn_state = HFP_STATE_CONNECTED;
            memcpy(s_peer_addr, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            s_peer_addr_valid = true;
            save_peer_to_nvs(s_peer_addr);
            bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_CONNECTING);
            if (s_target_name[0]) {
                esp_bt_gap_read_remote_name(s_peer_addr);
            }
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_CONNECTED);
            }
            break;

        case ESP_HF_CONNECTION_STATE_SLC_CONNECTED:
            s_scanning = false;
            esp_bt_gap_cancel_discovery();
            if (s_connect_watchdog) {
                xTimerStop(s_connect_watchdog, 0);
            }
            s_conn_state = HFP_STATE_SLC_CONNECTED;
            memcpy(s_peer_addr, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            s_peer_addr_valid = true;
            save_peer_to_nvs(s_peer_addr);
            bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_CONNECTED);
            esp_hf_ag_bsir(s_peer_addr, ESP_HF_IN_BAND_RINGTONE_NOT_PROVIDED);
            ESP_LOGI(TAG, "HFP AG SLC established");
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_SLC_CONNECTED);
            }
            break;

        case ESP_HF_CONNECTION_STATE_DISCONNECTED:
            ESP_LOGW(TAG, "HFP AG disconnected");
            if (s_connect_watchdog) {
                xTimerStop(s_connect_watchdog, 0);
            }
            stop_ring_timer();
            sco_tx_timer_stop();
            s_call_phase = AG_CALL_IDLE;
            s_conn_state = HFP_STATE_DISCONNECTED;
            bt_scan_set_link(param->conn_stat.remote_bda, BT_SCAN_LINK_NONE);
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_DISCONNECTED);
            }
            start_reconnect();
            break;

        default:
            break;
        }
        break;
    }

    case ESP_HF_AUDIO_STATE_EVT: {
        esp_hf_audio_state_t audio_state = param->audio_stat.state;
        ESP_LOGD(TAG, "Audio state: %d", audio_state);
        if (audio_state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
            s_sco_codec = SCO_CODEC_MSBC;
            if (s_callbacks.on_audio_codec_changed) {
                s_callbacks.on_audio_codec_changed(SCO_CODEC_MSBC);
            }
            sco_ring_reset_and_prefill();
            sco_tx_timer_start();
        } else if (audio_state == ESP_HF_AUDIO_STATE_CONNECTED) {
            s_sco_codec = SCO_CODEC_CVSD;
            if (s_callbacks.on_audio_codec_changed) {
                s_callbacks.on_audio_codec_changed(SCO_CODEC_CVSD);
            }
            sco_ring_reset_and_prefill();
            sco_tx_timer_start();
        } else if (audio_state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
            sco_tx_timer_stop();
        }
        break;
    }

    case ESP_HF_CIND_RESPONSE_EVT: {
        esp_hf_call_status_t call;
        esp_hf_call_setup_status_t setup;
        fill_cind(&call, &setup);
        esp_hf_ag_cind_response(param->cind_rep.remote_addr, call, setup,
                                ESP_HF_NETWORK_STATE_AVAILABLE, 5,
                                ESP_HF_ROAMING_STATUS_INACTIVE, 5,
                                ESP_HF_CALL_HELD_STATUS_NONE);
        break;
    }

    case ESP_HF_IND_UPDATE_EVT: {
        esp_hf_call_status_t call;
        esp_hf_call_setup_status_t setup;
        fill_cind(&call, &setup);
        esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_CALL, (int)call);
        esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_CALLSETUP, (int)setup);
        esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SERVICE,
                              ESP_HF_NETWORK_STATE_AVAILABLE);
        esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_SIGNAL, 5);
        esp_hf_ag_ciev_report(param->ind_upd.remote_addr, ESP_HF_IND_TYPE_BATTCHG, 5);
        break;
    }

    case ESP_HF_COPS_RESPONSE_EVT: {
        static char op[] = "Qwen";
        esp_hf_ag_cops_response(param->cops_rep.remote_addr, op);
        break;
    }

    case ESP_HF_CNUM_RESPONSE_EVT: {
        const system_config_t *cfg = config_get();
        static char num[32] = "0";
        if (cfg && cfg->bt_device_name[0]) {
            strncpy(num, cfg->bt_device_name, sizeof(num) - 1);
            num[sizeof(num) - 1] = '\0';
        }
        esp_hf_ag_cnum_response(param->cnum_rep.remote_addr, num, 129,
                                ESP_HF_SUBSCRIBER_SERVICE_TYPE_VOICE);
        break;
    }

    case ESP_HF_CLCC_RESPONSE_EVT: {
        if (s_call_phase == AG_CALL_IDLE) {
            static char empty[] = "";
            esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 0, 0, 0, 0, 0,
                                    empty, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
            break;
        }
        esp_hf_current_call_direction_t dir =
            (s_call_phase == AG_CALL_INCOMING)
                ? ESP_HF_CURRENT_CALL_DIRECTION_INCOMING
                : ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING;
        esp_hf_current_call_status_t st;
        switch (s_call_phase) {
        case AG_CALL_INCOMING:
            st = ESP_HF_CURRENT_CALL_STATUS_INCOMING;
            break;
        case AG_CALL_DIALING:
            st = ESP_HF_CURRENT_CALL_STATUS_DIALING;
            break;
        case AG_CALL_ALERTING:
            st = ESP_HF_CURRENT_CALL_STATUS_ALERTING;
            break;
        default:
            st = ESP_HF_CURRENT_CALL_STATUS_ACTIVE;
            break;
        }
        char *num = s_call_number;
        esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 1, dir, st,
                                ESP_HF_CURRENT_CALL_MODE_VOICE, ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE,
                                num, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        esp_hf_ag_clcc_response(param->clcc_rep.remote_addr, 0, dir, st,
                                ESP_HF_CURRENT_CALL_MODE_VOICE, ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE,
                                num, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
        break;
    }

    case ESP_HF_UNAT_RESPONSE_EVT: {
        const char *at = param->unat_rep.unat ? param->unat_rep.unat : "";
        ESP_LOGI(TAG, "Unknown AT from headset: %s", at);
        /* HSP button: ring → answer, in-call → hangup */
        if (strstr(at, "CKPD") || strstr(at, "ckpd")) {
            esp_hf_ag_cmee_send(param->unat_rep.remote_addr,
                                ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            if (s_call_phase == AG_CALL_INCOMING && s_callbacks.on_hf_answer) {
                s_callbacks.on_hf_answer();
            } else if (s_call_phase != AG_CALL_IDLE && s_callbacks.on_hf_hangup) {
                s_callbacks.on_hf_hangup();
            }
            break;
        }
        /* Some units send CHLD=0 (UDUB) instead of CHUP to reject ringing. */
        if (s_call_phase == AG_CALL_INCOMING &&
            (strstr(at, "CHLD=0") || strstr(at, "chld=0"))) {
            esp_hf_ag_cmee_send(param->unat_rep.remote_addr,
                                ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
            if (s_callbacks.on_hf_hangup) {
                s_callbacks.on_hf_hangup();
            }
            break;
        }
        esp_hf_ag_unknown_at_send(param->unat_rep.remote_addr, NULL);
        break;
    }

    case ESP_HF_VOLUME_CONTROL_EVT:
        ESP_LOGD(TAG, "Volume: type=%d vol=%d",
                 param->volume_control.type, param->volume_control.volume);
        break;

    case ESP_HF_BVRA_RESPONSE_EVT:
        esp_hf_ag_vra_control(param->vra_rep.remote_addr, ESP_HF_VR_STATE_DISABLED);
        break;

    case ESP_HF_NREC_RESPONSE_EVT:
        ESP_LOGD(TAG, "NREC: %d", param->nrec.state);
        break;

    case ESP_HF_ATA_RESPONSE_EVT:
        ESP_LOGI(TAG, "Headset answered (ATA)");
        if (s_callbacks.on_hf_answer) {
            s_callbacks.on_hf_answer();
        }
        break;

    case ESP_HF_CHUP_RESPONSE_EVT:
        ESP_LOGI(TAG, "Headset hangup (CHUP)");
        if (s_callbacks.on_hf_hangup) {
            s_callbacks.on_hf_hangup();
        }
        break;

    case ESP_HF_DIAL_EVT: {
        const char *num = NULL;
        if (param->out_call.type == ESP_HF_DIAL_NUM && param->out_call.num_or_loc) {
            num = param->out_call.num_or_loc;
            ESP_LOGI(TAG, "Headset dial: %s", num);
        } else if (param->out_call.type == ESP_HF_DIAL_MEM) {
            ESP_LOGI(TAG, "Headset memory dial (unsupported), using last number");
            num = s_last_number;
        } else {
            ESP_LOGI(TAG, "Headset redial last number");
            num = s_last_number;
        }

        if (num && num[0]) {
            esp_hf_ag_cmee_send(param->out_call.remote_addr,
                                ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        } else {
            /* Empty number is still delivered to the gateway (uses forward number). */
            esp_hf_ag_cmee_send(param->out_call.remote_addr,
                                ESP_HF_AT_RESPONSE_CODE_OK, ESP_HF_CME_AG_FAILURE);
        }
        if (s_callbacks.on_hf_dial) {
            s_callbacks.on_hf_dial(num ? num : "");
        }
        break;
    }

    case ESP_HF_WBS_RESPONSE_EVT:
        ESP_LOGD(TAG, "WBS codec event: %d", param->wbs_rep.codec);
        break;

    case ESP_HF_BCS_RESPONSE_EVT:
        ESP_LOGD(TAG, "Codec negotiation result: %d", param->bcs_rep.mode);
        break;

    case ESP_HF_PROF_STATE_EVT:
        ESP_LOGI(TAG, "AG profile state: %d", param->prof_stat.state);
        break;

    default:
        ESP_LOGD(TAG, "Unhandled AG event: %d", event);
        break;
    }
}

static void ag_incoming_data_cb(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0 || !s_sco_tx_active) {
        return;
    }
    if (s_callbacks.on_audio_data) {
        s_callbacks.on_audio_data(buf, len);
    }
}

static uint32_t ag_outgoing_data_cb(uint8_t *buf, uint32_t len)
{
    /* Returning a full frame while SCO is CLOSING makes Bluedroid loop
     * forever in bta_ag_sco_event (SBC encode on BTU_TASK → task WDT). */
    if (!s_sco_tx_active || s_sco_out_buf == NULL || buf == NULL || len == 0) {
        return 0;
    }

    s_sco_frame_len = len;

    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t avail = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);
    uint32_t copy = (avail < (uint16_t)len) ? avail : (uint16_t)len;
    if (copy > 0) {
        uint16_t to_end = SCO_OUT_RING_SIZE - s_sco_out_rd;
        if (copy <= to_end) {
            memcpy(buf, s_sco_out_buf + s_sco_out_rd, copy);
        } else {
            memcpy(buf, s_sco_out_buf + s_sco_out_rd, to_end);
            memcpy(buf + to_end, s_sco_out_buf, copy - to_end);
        }
        s_sco_out_rd = (uint16_t)((s_sco_out_rd + copy) & SCO_OUT_MASK);
    }
    portEXIT_CRITICAL(&s_sco_out_mux);

    if (copy < len) {
        __atomic_add_fetch(&s_sco_pad_frames, 1, __ATOMIC_RELAXED);
        memset(buf + copy, 0, len - copy);
    }
    __atomic_add_fetch(&s_sco_cb_calls, 1, __ATOMIC_RELAXED);
    return len;
}
