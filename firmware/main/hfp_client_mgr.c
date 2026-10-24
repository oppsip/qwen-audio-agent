/**
 * HFP Client Manager - 蓝牙 HFP 客户端管理器
 *
 * 封装 ESP-IDF Bluedroid HFP Client API，管理蓝牙连接、电话控制和 SCO 语音数据。
 * 支持自动重连、来电指示监听、SCO 编码协商和双向语音数据传输。
 *
 * 适配 ESP-IDF v6.0 HFP Client API：
 * - 使用 esp_hf_client_register_data_callback() 注册语音收发回调（legacy HCI path）
 * - 接收：Bluedroid 内部解码 mSBC，回调提供 decoded PCM
 * - 发送：BT 栈通过 outgoing callback 拉取 PCM，写入 SCO TX
 */

#include "hfp_client_mgr.h"
#include "bt_scan.h"
#include "config_manager.h"
#include "audio_coex.h"

#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_hf_client_legacy_api.h"
#include "esp_timer.h"
#include "freertos/portmacro.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "hfp_mgr";

/* ---------- 内部状态 ---------- */

/** 当前 HFP 连接状态 */
static volatile hfp_conn_state_t s_conn_state = HFP_STATE_DISCONNECTED;

/** 当前 SCO 编码类型 */
static volatile sco_codec_t s_sco_codec = SCO_CODEC_CVSD;

/** 通话指示符：是否有活跃通话（call indicator = 1） */
static volatile bool s_call_active = false;

/** 呼叫建立指示符：当前 call_setup 状态，用于检测通话被挂断 */
static volatile esp_hf_call_setup_status_t s_call_setup_status = ESP_HF_CALL_SETUP_STATUS_IDLE;

/** 上层注册的回调 */
static hfp_callbacks_t s_callbacks;

/** 已配对设备的蓝牙地址（用于自动重连） */
static esp_bd_addr_t s_peer_addr;
static bool s_peer_addr_valid = false;
static bool s_auto_reconnect  = true;
static volatile bool s_hold_audio = false;

/** 当前 SCO 连接句柄（保留，目前仅用于日志） */
static esp_hf_sync_conn_hdl_t s_sync_conn_hdl = 0;

/** 自动重连定时器 */
static TimerHandle_t s_reconnect_timer = NULL;

/* CONNECTING 状态看门狗：若超过此时长仍未收到 CONNECTED/DISCONNECTED 回调，
 * 强制调用 esp_hf_client_disconnect() 清理 BT 栈悬挂状态，然后重新调度重连。
 * 设为 20s，大于任何正常 BT page 超时（通常 10~12s）。 */
#define CONNECT_WATCHDOG_MS  20000
static TimerHandle_t s_connect_watchdog = NULL;

/*
 * Outgoing SCO ring buffer — filled by hfp_push_audio() (audio bridge task),
 * drained by hfp_outgoing_data_cb() (BT task).  Power-of-2 size for cheap
 * masking.  HWM is selected at push time — see audio_path_sco_hwm().
 */
#define SCO_OUT_RING_SIZE  4096u   /* must be power of 2 */
#define SCO_OUT_MASK       (SCO_OUT_RING_SIZE - 1u)

/* Silence pre-fill on SCO connect: covers the gap before the first RTP frame
 * arrives (≤ 20 ms), giving the drain side something to play immediately.
 * One RTP frame (160 samples @ 8kHz → 320 samples @ 16kHz → 640 bytes). */
#define SCO_OUT_PREFILL    640u

static uint8_t *s_sco_out_buf = NULL;
static uint16_t s_sco_out_wr = 0;
static uint16_t s_sco_out_rd = 0;
static portMUX_TYPE s_sco_out_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_sco_cb_calls;
static uint32_t s_sco_pad_frames;
static uint32_t s_sco_trims;

/* Bytes Bluedroid asks for per SCO frame; learned from the first callback. */
static volatile uint32_t s_sco_frame_len = 0;

/*
 * SCO TX pump timer — fires every 7.5 ms (one mSBC frame interval) and calls
 * esp_hf_client_outgoing_data_ready() to trigger hfp_outgoing_data_cb().
 * A self-driving chain (calling outgoing_data_ready from inside the callback)
 * causes Bluedroid's BTC task to spin in a tight loop starving IDLE — the
 * 7.5ms timer is the correct throttle mechanism.
 */
static esp_timer_handle_t s_sco_tx_timer = NULL;

/* 重连延迟：足够长让 BT 控制器彻底清理 ACL/RFCOMM 状态，
 * 同时让手机侧自动重连（通常 5s 内）优先发起，避免双向同时建链。 */
#define RECONNECT_DELAY_MS  8000

/* ---------- 前向声明 ---------- */

static void hfp_client_cb(esp_hf_client_cb_event_t event,
                           esp_hf_client_cb_param_t *param);
static void hfp_incoming_data_cb(const uint8_t *buf, uint32_t len);
static uint32_t hfp_outgoing_data_cb(uint8_t *buf, uint32_t len);
static void start_reconnect(void);
static void reconnect_timer_cb(TimerHandle_t timer);
static void connect_watchdog_cb(TimerHandle_t timer);

/* ---------- NVS 持久化：上次配对设备地址 ---------- */

static void save_peer_to_nvs(const esp_bd_addr_t addr)
{
    nvs_handle_t h;
    if (nvs_open("hfp_mgr", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "peer_addr", addr, sizeof(esp_bd_addr_t));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Peer BD_ADDR saved to NVS");
}

static bool load_peer_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open("hfp_mgr", NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(esp_bd_addr_t);
    esp_err_t err = nvs_get_blob(h, "peer_addr", s_peer_addr, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(esp_bd_addr_t)) {
        s_peer_addr_valid = true;
        ESP_LOGI(TAG, "Loaded peer BD_ADDR from NVS: " ESP_BD_ADDR_STR,
                 ESP_BD_ADDR_HEX(s_peer_addr));
        return true;
    }
    return false;
}

static void sco_tx_pump_cb(void *arg)
{
    (void)arg;
    esp_hf_client_outgoing_data_ready();

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
        esp_hf_client_outgoing_data_ready();
    }
}

static void sco_tx_timer_start(void)
{
    if (s_sco_tx_timer != NULL) {
        return;
    }
    esp_timer_create_args_t ta = {
        .callback             = sco_tx_pump_cb,
        .name                 = "sco_tx",
        .dispatch_method      = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&ta, &s_sco_tx_timer) == ESP_OK) {
        esp_timer_start_periodic(s_sco_tx_timer, 7500);  /* 7.5 ms */
        ESP_LOGD(TAG, "SCO TX pump timer started");
    }
}

static void sco_tx_timer_stop(void)
{
    if (s_sco_tx_timer == NULL) {
        return;
    }
    esp_timer_stop(s_sco_tx_timer);
    esp_timer_delete(s_sco_tx_timer);
    s_sco_tx_timer = NULL;
    ESP_LOGD(TAG, "SCO TX pump timer stopped");
}

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
    case ESP_BT_GAP_DISC_RES_EVT:
        bt_scan_on_disc_res(param, NULL, 0);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        bt_scan_on_disc_state(param);
        break;
    case ESP_BT_GAP_READ_REMOTE_NAME_EVT:
        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            bt_scan_on_remote_name(param->read_rmt_name.bda,
                                   (const char *)param->read_rmt_name.rmt_name);
        }
        break;
    default:
        break;
    }
}

/* ================================================================
 * Task 8.1 - HFP 初始化和连接管理
 * ================================================================ */

esp_err_t hfp_client_init(const char *device_name, const hfp_callbacks_t *callbacks)
{
    esp_err_t ret;

    if (device_name == NULL || callbacks == NULL) {
        ESP_LOGE(TAG, "Invalid arguments: device_name or callbacks is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_callbacks, callbacks, sizeof(hfp_callbacks_t));
    bt_scan_set_role(BT_MODE_HFP);

    /* Allocate SCO ring buffer from PSRAM to keep DRAM heap free */
    s_sco_out_buf = heap_caps_malloc(SCO_OUT_RING_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_sco_out_buf == NULL) {
        ESP_LOGE(TAG, "Failed to allocate SCO ring buffer from DRAM");
        return ESP_ERR_NO_MEM;
    }
    /* --- 初始化蓝牙控制器 --- */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* sdkconfig uses BTDM firmware — must enable with BTDM even in HFP-only mode.
     * BLE remains idle; only Classic BT callbacks (HFP) are registered. */
    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * Disable BT modem sleep.  With sleep enabled (sdkconfig default), the BT
     * controller may doze between ACL packets and wake up too late to honour
     * the fixed 7.5 ms SCO slot grid, producing audio gaps and noise.
     * For a voice-call gateway there is no benefit to BT sleep.
     */
    esp_bt_sleep_disable();


    /* --- 初始化 Bluedroid --- */
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

    /* --- 设置设备名称 --- */
    ret = esp_bt_gap_set_device_name(device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_bt_gap_register_callback(gap_cb);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GAP callback register failed: %s", esp_err_to_name(ret));
    }

    /* --- 注册 HFP Client 回调并初始化 --- */
    ret = esp_hf_client_register_callback(hfp_client_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HFP client register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_hf_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HFP client init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Init forces msbc_enabled=TRUE; re-apply so WiFi advertises CVSD only. */
    audio_coex_apply_wbs_preference();

    /* Register SCO audio data callbacks (legacy HCI path, no external codec).
     * Bluedroid decodes mSBC internally and delivers decoded PCM via recv cb.
     * send cb is called by the BT stack to fetch outgoing PCM for the SCO tx. */
    ret = esp_hf_client_register_data_callback(hfp_incoming_data_cb, hfp_outgoing_data_cb);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Register data callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- 进入可发现和可连接状态 --- */
    ret = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set scan mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* --- 创建自动重连定时器（单次触发） --- */
    s_reconnect_timer = xTimerCreate("hfp_reconn",
                                     pdMS_TO_TICKS(RECONNECT_DELAY_MS),
                                     pdFALSE,  /* 单次触发 */
                                     NULL,
                                     reconnect_timer_cb);
    if (s_reconnect_timer == NULL) {
        ESP_LOGW(TAG, "Failed to create reconnect timer");
    }

    s_connect_watchdog = xTimerCreate("hfp_wdog",
                                      pdMS_TO_TICKS(CONNECT_WATCHDOG_MS),
                                      pdFALSE,  /* 单次触发 */
                                      NULL,
                                      connect_watchdog_cb);
    if (s_connect_watchdog == NULL) {
        ESP_LOGW(TAG, "Failed to create connect watchdog timer");
    }

    /* 从 NVS 恢复上次配对设备地址，上电后立即开始自动重连 */
    if (load_peer_from_nvs()) {
        start_reconnect();
    }

    ESP_LOGI(TAG, "HFP client initialized, device name: %s", device_name);
    return ESP_OK;
}

hfp_conn_state_t hfp_get_conn_state(void)
{
    return s_conn_state;
}

bool hfp_incoming_call_pending(void)
{
    return s_call_setup_status == ESP_HF_CALL_SETUP_STATUS_INCOMING && !s_call_active;
}

bool hfp_call_is_active(void)
{
    return s_call_active;
}

/* ---------- 自动重连 ---------- */

/**
 * 重连定时器回调：尝试重新连接已配对设备
 */
static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;

    if (!s_peer_addr_valid) {
        ESP_LOGW(TAG, "No peer address for reconnect");
        return;
    }

    if (s_conn_state != HFP_STATE_DISCONNECTED) {
        ESP_LOGD(TAG, "Not disconnected, skip reconnect");
        return;
    }

    ESP_LOGI(TAG, "Attempting auto-reconnect to " ESP_BD_ADDR_STR,
             ESP_BD_ADDR_HEX(s_peer_addr));

    s_conn_state = HFP_STATE_CONNECTING;
    bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_CONNECTING);
    if (s_callbacks.on_connection_state) {
        s_callbacks.on_connection_state(HFP_STATE_CONNECTING);
    }

    esp_err_t ret = esp_hf_client_connect(s_peer_addr);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Reconnect esp_hf_client_connect failed: %s",
                 esp_err_to_name(ret));
        s_conn_state = HFP_STATE_DISCONNECTED;
        bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_NONE);
        if (s_callbacks.on_connection_state) {
            s_callbacks.on_connection_state(HFP_STATE_DISCONNECTED);
        }
        /* 再次尝试 */
        if (s_reconnect_timer) {
            xTimerStart(s_reconnect_timer, 0);
        }
    }
}

void hfp_set_auto_reconnect(bool enable)
{
    s_auto_reconnect = enable;
    if (!enable && s_reconnect_timer) {
        xTimerStop(s_reconnect_timer, 0);
    }
    ESP_LOGI(TAG, "Auto-reconnect: %s", enable ? "enabled" : "disabled");
}

void hfp_hold_audio(bool hold)
{
    s_hold_audio = hold;
    if (hold && s_sync_conn_hdl && s_peer_addr_valid) {
        ESP_LOGI(TAG, "Whitelist call: disconnecting existing SCO");
        sco_tx_timer_stop();
        esp_hf_client_disconnect_audio(s_peer_addr);
        s_sync_conn_hdl = 0;
    }
}

bool hfp_get_peer_bda(uint8_t bda[6])
{
    if (bda == NULL || !s_peer_addr_valid) {
        return false;
    }
    memcpy(bda, s_peer_addr, 6);
    return true;
}

/**
 * 启动自动重连定时器。
 * 同时重新确认 ESP32 处于可连接+可发现状态，确保手机侧也能主动发起回连。
 */
static void start_reconnect(void)
{
    if (!s_auto_reconnect) {
        ESP_LOGI(TAG, "Auto-reconnect disabled, skip");
        return;
    }

    /* 重新声明可连接/可发现：距离过远导致断连后，手机的 page scan 可能
     * 已降频或停止。刷新此设置让手机重新发现 ESP32，使手机侧也能主动重连。 */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);

    if (s_reconnect_timer && s_peer_addr_valid) {
        ESP_LOGI(TAG, "Scheduling reconnect in %d ms", RECONNECT_DELAY_MS);
        xTimerStart(s_reconnect_timer, 0);
    }
}

/**
 * CONNECTING 状态看门狗回调：
 * 若超过 CONNECT_WATCHDOG_MS 仍未收到 CONNECTED/DISCONNECTED 事件，
 * 说明 Bluedroid 内部状态悬挂。强制清理后重新调度重连。
 */
static void connect_watchdog_cb(TimerHandle_t timer)
{
    (void)timer;
    ESP_LOGW(TAG, "Connect watchdog fired — BT stack stuck in CONNECTING, forcing recovery");

    /* 强制调用 disconnect 清理 BT 栈内部悬挂的 ACL/RFCOMM 状态 */
    if (s_peer_addr_valid) {
        esp_hf_client_disconnect(s_peer_addr);
    }

    s_conn_state = HFP_STATE_DISCONNECTED;
    if (s_peer_addr_valid) {
        bt_scan_set_link(s_peer_addr, BT_SCAN_LINK_NONE);
    }
    if (s_callbacks.on_connection_state) {
        s_callbacks.on_connection_state(HFP_STATE_DISCONNECTED);
    }

    /* 等 BT 栈消化完 disconnect，然后重新调度连接 */
    start_reconnect();
}

/* ================================================================
 * Task 8.2 - HFP 电话控制和语音数据
 * ================================================================ */

esp_err_t hfp_answer_call(void)
{
    if (s_conn_state < HFP_STATE_SLC_CONNECTED) {
        ESP_LOGE(TAG, "Cannot answer call: HFP not connected (state=%d)", s_conn_state);
        return ESP_ERR_INVALID_STATE;
    }
    if (!hfp_incoming_call_pending()) {
        ESP_LOGW(TAG, "Cannot answer call: no incoming call is pending");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Answering call (ATA)");
    esp_err_t ret = esp_hf_client_answer_call();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hf_client_answer_call failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t hfp_hangup(void)
{
    if (s_conn_state < HFP_STATE_SLC_CONNECTED) {
        ESP_LOGE(TAG, "Cannot hangup: HFP not connected (state=%d)", s_conn_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Hanging up call (AT+CHUP)");
    esp_err_t ret = esp_hf_client_reject_call();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hf_client_reject_call failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t hfp_dial(const char *number)
{
    if (number == NULL || number[0] == '\0') {
        ESP_LOGE(TAG, "Invalid dial number");
        return ESP_ERR_INVALID_ARG;
    }

    if (s_conn_state < HFP_STATE_SLC_CONNECTED) {
        ESP_LOGE(TAG, "Cannot dial: HFP not connected (state=%d)", s_conn_state);
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Dialing number: %s (ATD)", number);
    esp_err_t ret = esp_hf_client_dial(number);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_hf_client_dial failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

void hfp_push_audio(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    uint16_t trim = 0;

    uint16_t hwm = audio_path_sco_hwm(s_sco_codec);

    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t occupied = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);

    /* Trim oldest audio when the buffer is over the high-water mark.
     * Advancing rd discards stale frames; the incoming frame is written in
     * full.  This is preferable to dropping the incoming frame (which would
     * repeat the over-full condition and keep losing new audio). */
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
        s_sco_out_wr = (s_sco_out_wr + (uint16_t)len) & SCO_OUT_MASK;
    }
    portEXIT_CRITICAL(&s_sco_out_mux);

    if (trim > 0) {
        __atomic_add_fetch(&s_sco_trims, 1, __ATOMIC_RELAXED);
        static uint32_t s_trim_count = 0;
        s_trim_count++;
        if (s_trim_count <= 5 || (s_trim_count % 100) == 0) {
            ESP_LOGD(TAG, "SCO out HWM trim #%lu: discarded %u stale bytes",
                     (unsigned long)s_trim_count, trim);
        }
    }
}

sco_codec_t hfp_get_sco_codec(void)
{
    return s_sco_codec;
}

int hfp_sco_out_level_ms(void)
{
    if (s_sco_out_buf == NULL || s_sco_tx_timer == NULL) {
        return -1;
    }
    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t occupied = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);
    portEXIT_CRITICAL(&s_sco_out_mux);

    /* PCM bytes/ms: mSBC 16 kHz × 2 = 32, CVSD 8 kHz × 2 = 16 */
    unsigned bpm = (s_sco_codec == SCO_CODEC_MSBC) ? 32u : 16u;
    return (int)(occupied / bpm);
}

void hfp_sco_tx_stats(uint32_t *calls, uint32_t *pad, uint32_t *trims)
{
    if (calls) *calls = __atomic_load_n(&s_sco_cb_calls, __ATOMIC_RELAXED);
    if (pad) *pad = __atomic_load_n(&s_sco_pad_frames, __ATOMIC_RELAXED);
    if (trims) *trims = __atomic_load_n(&s_sco_trims, __ATOMIC_RELAXED);
}

/* ================================================================
 * HFP Client 事件回调（内部）
 * ================================================================ */

/**
 * HFP Client 主回调函数
 * 处理连接状态、来电指示、通话状态、SCO 编码协商等事件
 */
static void hfp_client_cb(esp_hf_client_cb_event_t event,
                           esp_hf_client_cb_param_t *param)
{
    switch (event) {

    /* --- 连接状态事件 --- */
    case ESP_HF_CLIENT_CONNECTION_STATE_EVT: {
        esp_hf_client_connection_state_t state = param->conn_stat.state;
        ESP_LOGI(TAG, "Connection state: %d, peer: " ESP_BD_ADDR_STR,
                 state, ESP_BD_ADDR_HEX(param->conn_stat.remote_bda));

        switch (state) {
        case ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING:
            /* AT+BAC is sent during SLC; advertise CVSD-only on WiFi first. */
            audio_coex_apply_wbs_preference();
            /* 无论是主动发起还是手机侧发起，连接已在进行中。
             * 停止重连定时器防止并发建链，同时启动看门狗：
             * 若超过 CONNECT_WATCHDOG_MS 没有后续回调说明 BT 栈悬挂，需强制恢复。 */
            if (s_reconnect_timer) {
                xTimerStop(s_reconnect_timer, 0);
            }
            if (s_connect_watchdog) {
                xTimerStart(s_connect_watchdog, 0);
            }
            s_conn_state = HFP_STATE_CONNECTING;
            bt_scan_set_link(param->conn_stat.remote_bda, BT_SCAN_LINK_CONNECTING);
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_CONNECTING);
            }
            break;

        case ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED:
            /* ACL + RFCOMM 已建立，等待 SLC。停止所有定时器。 */
            audio_coex_apply_wbs_preference();
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
            ESP_LOGI(TAG, "HFP connected, peer saved");
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_CONNECTED);
            }
            break;

        case ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED:
            if (s_connect_watchdog) {
                xTimerStop(s_connect_watchdog, 0);
            }
            s_conn_state = HFP_STATE_SLC_CONNECTED;
            bt_scan_set_link(param->conn_stat.remote_bda, BT_SCAN_LINK_CONNECTED);
            ESP_LOGI(TAG, "HFP SLC established");
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(HFP_STATE_SLC_CONNECTED);
            }
            break;

        case ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED:
            ESP_LOGW(TAG, "HFP disconnected");
            if (s_connect_watchdog) {
                xTimerStop(s_connect_watchdog, 0);
            }
            s_conn_state = HFP_STATE_DISCONNECTED;
            s_call_active = false;
            s_call_setup_status = ESP_HF_CALL_SETUP_STATUS_IDLE;
            s_hold_audio = false;
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

    /* --- SCO 音频连接状态和编码协商 --- */
    case ESP_HF_CLIENT_AUDIO_STATE_EVT: {
        esp_hf_client_audio_state_t audio_state = param->audio_stat.state;
        ESP_LOGD(TAG, "Audio state: %d, sync_conn_handle: 0x%04x",
                 audio_state, param->audio_stat.sync_conn_handle);

        if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC ||
            audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            if (s_hold_audio) {
                ESP_LOGI(TAG, "Whitelist call: dropping SCO so audio stays on the phone");
                sco_tx_timer_stop();
                s_sync_conn_hdl = 0;
                if (s_peer_addr_valid) {
                    esp_hf_client_disconnect_audio(s_peer_addr);
                }
                break;
            }
        }
        if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC) {
            s_sco_codec = SCO_CODEC_MSBC;
            s_sync_conn_hdl = param->audio_stat.sync_conn_handle;
            ESP_LOGD(TAG, "SCO audio connected with mSBC (16kHz)%s",
                     audio_path_on_wifi()
                     ? " — WiFi asked for CVSD-only; phone still chose WBS"
                     : "");
            if (s_callbacks.on_audio_codec_changed) {
                s_callbacks.on_audio_codec_changed(SCO_CODEC_MSBC);
            }
            portENTER_CRITICAL(&s_sco_out_mux);
            s_sco_out_wr = 0;
            s_sco_out_rd = 0;
            memset(s_sco_out_buf, 0, SCO_OUT_PREFILL);
            s_sco_out_wr = SCO_OUT_PREFILL;
            portEXIT_CRITICAL(&s_sco_out_mux);
            ESP_LOGD(TAG, "SCO ring pre-filled %u bytes silence", SCO_OUT_PREFILL);
            sco_tx_timer_start();
        } else if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_CONNECTED) {
            s_sco_codec = SCO_CODEC_CVSD;
            s_sync_conn_hdl = param->audio_stat.sync_conn_handle;
            ESP_LOGD(TAG, "SCO audio connected with CVSD (8kHz)");
            if (s_callbacks.on_audio_codec_changed) {
                s_callbacks.on_audio_codec_changed(SCO_CODEC_CVSD);
            }
            portENTER_CRITICAL(&s_sco_out_mux);
            s_sco_out_wr = 0;
            s_sco_out_rd = 0;
            memset(s_sco_out_buf, 0, SCO_OUT_PREFILL);
            s_sco_out_wr = SCO_OUT_PREFILL;
            portEXIT_CRITICAL(&s_sco_out_mux);
            ESP_LOGD(TAG, "SCO ring pre-filled %u bytes silence", SCO_OUT_PREFILL);
            sco_tx_timer_start();
        } else if (audio_state == ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED) {
            ESP_LOGD(TAG, "SCO audio disconnected");
            s_sync_conn_hdl = 0;
            sco_tx_timer_stop();
        }
        break;
    }

    /* --- 来电 RING 指示 --- */
    case ESP_HF_CLIENT_RING_IND_EVT:
        ESP_LOGI(TAG, "RING indication (incoming call)");
        /* 来电号码通过 CLIP 事件获取 */
        break;

    /* --- 来电号码 (CLIP) --- */
    case ESP_HF_CLIENT_CLIP_EVT:
        ESP_LOGI(TAG, "Incoming call from: %s", param->clip.number);
        if (s_callbacks.on_incoming_call) {
            s_callbacks.on_incoming_call(param->clip.number);
        }
        break;

    /* --- 通话状态变化 --- */
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        ESP_LOGI(TAG, "Call indicator: %d", param->call.status);
        if (param->call.status == ESP_HF_CALL_STATUS_NO_CALLS) {
            s_call_active = false;
            s_hold_audio = false;
            /* 通话结束 */
            if (s_callbacks.on_call_ended) {
                s_callbacks.on_call_ended();
            }
        } else if (param->call.status == ESP_HF_CALL_STATUS_CALL_IN_PROGRESS) {
            s_call_active = true;
            /*
             * 通话进行中 - 可能是来电接听成功或呼出对方接听。
             * on_call_answered 用于来电接听确认。
             * on_outgoing_call_connected 用于呼出对方接听确认。
             * Gateway Controller 根据当前呼叫方向选择处理。
             */
            if (s_callbacks.on_call_answered) {
                s_callbacks.on_call_answered();
            }
            if (s_callbacks.on_outgoing_call_connected) {
                s_callbacks.on_outgoing_call_connected();
            }
        }
        break;

    /* --- 通话建立状态 --- */
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        ESP_LOGI(TAG, "Call setup indicator: %d", param->call_setup.status);
        {
            esp_hf_call_setup_status_t prev_setup = s_call_setup_status;
            s_call_setup_status = param->call_setup.status;

            switch (param->call_setup.status) {
            case ESP_HF_CALL_SETUP_STATUS_IDLE:
                /*
                 * 呼叫建立阶段结束。若之前处于建立中状态（来电或呼出），
                 * 且此时无活跃通话，说明通话被挂断（来电被拒或呼出未接）。
                 * 此时 CIND_CALL_EVT 不一定会触发（call 指示符未变化），
                 * 需在此主动通知上层结束通话，以便发送 SIP CANCEL。
                 */
                if (prev_setup != ESP_HF_CALL_SETUP_STATUS_IDLE && !s_call_active) {
                    s_hold_audio = false;
                    if (s_callbacks.on_call_ended) {
                        s_callbacks.on_call_ended();
                    }
                }
                break;
            case ESP_HF_CALL_SETUP_STATUS_INCOMING:
                ESP_LOGI(TAG, "Incoming call setup");
                break;
            case ESP_HF_CALL_SETUP_STATUS_OUTGOING_DIALING:
                ESP_LOGI(TAG, "Outgoing call setup");
                break;
            case ESP_HF_CALL_SETUP_STATUS_OUTGOING_ALERTING:
                ESP_LOGI(TAG, "Outgoing call alerting (ringing at remote)");
                break;
            default:
                break;
            }
        }
        break;

    /* --- 通话保持状态 --- */
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        ESP_LOGI(TAG, "Call held indicator: %d", param->call_held.status);
        break;

    /* --- AT 命令响应 --- */
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        if (param->at_response.code != ESP_HF_AT_RESPONSE_CODE_OK) {
            ESP_LOGW(TAG, "AT response error: code=%d, cme=%d",
                     param->at_response.code, param->at_response.cme);
            /*
             * 如果拨号失败（AT 响应错误），通知上层
             * 在呼出场景中，call_setup 从 OUTGOING 变为 NONE 且无 CALL_IN_PROGRESS
             * 表示呼出失败
             */
            if (s_callbacks.on_outgoing_call_failed) {
                s_callbacks.on_outgoing_call_failed();
            }
        }
        break;

    /* --- SCO 编码协商 (Codec Negotiation) --- */
    case ESP_HF_CLIENT_BSIR_EVT:
        ESP_LOGI(TAG, "In-band ring tone: %s",
                 param->bsir.state ? "enabled" : "disabled");
        break;

    /* --- 音量变化 --- */
    case ESP_HF_CLIENT_VOLUME_CONTROL_EVT:
        ESP_LOGD(TAG, "Volume control: type=%d, volume=%d",
                 param->volume_control.type, param->volume_control.volume);
        break;

    default:
        ESP_LOGD(TAG, "Unhandled HFP event: %d", event);
        break;
    }
}

/* ================================================================
 * SCO 语音数据回调（内部） - ESP-IDF v6.0 API
 * ================================================================ */

/**
 * hfp_incoming_data_cb — called by Bluedroid (legacy HCI path) with decoded PCM.
 * CVSD: 8 kHz PCM.  mSBC: 16 kHz PCM (decoded internally by Bluedroid).
 * Buffer is owned by the stack and is valid only for the duration of this call.
 */
static void hfp_incoming_data_cb(const uint8_t *buf, uint32_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (s_callbacks.on_audio_data) {
        s_callbacks.on_audio_data(buf, len);
    }
}

/*
 * hfp_outgoing_data_cb — called by Bluedroid when it needs PCM to encode and
 * send over SCO.  Must be non-blocking; return 0 if no data is available.
 * Drains s_sco_out_buf ring buffer filled by hfp_push_audio().
 */
static uint32_t hfp_outgoing_data_cb(uint8_t *buf, uint32_t len)
{
    static uint32_t s_cb_count = 0;

    s_sco_frame_len = len;

    portENTER_CRITICAL(&s_sco_out_mux);
    uint16_t avail = (uint16_t)((s_sco_out_wr - s_sco_out_rd) & SCO_OUT_MASK);
    uint32_t copy  = (avail < (uint16_t)len) ? avail : (uint16_t)len;
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

    s_cb_count++;
    if (s_cb_count == 1 || (s_cb_count % 200) == 0) {
        ESP_LOGD(TAG, "SCO out #%lu: copy=%lu/%lu avail_was=%u",
                 (unsigned long)s_cb_count, (unsigned long)copy,
                 (unsigned long)len, avail);
    }

    if (copy < len) {
        __atomic_add_fetch(&s_sco_pad_frames, 1, __ATOMIC_RELAXED);
        memset(buf + copy, 0, len - copy);
    }
    __atomic_add_fetch(&s_sco_cb_calls, 1, __ATOMIC_RELAXED);
    return len;
}
