/**
 * A2DP Source Manager
 *
 * GAP inquiry 扫描目标设备（按名称匹配），连接后以 A2DP Source 角色流式
 * 发送音频到蓝牙音箱。音频输入为 8 kHz 单声道 PCM，内部通过线性插值
 * 升采样至 44100 Hz，再复制为立体声供 SBC 编码器消费。
 *
 * 连接断开后自动重扫重连，媒体控制握手（CHECK_SRC_RDY → START）由
 * A2DP 回调驱动，用 esp_timer 完成延迟重试。
 */

#include "a2dp_mgr.h"
#include "bt_scan.h"
#include "config_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "a2dp_mgr";

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */
static volatile a2dp_conn_state_t s_state = A2DP_STATE_DISCONNECTED;
static a2dp_callbacks_t           s_callbacks;
static char                       s_target_name[32];
static esp_bd_addr_t              s_peer_bda;

/* ------------------------------------------------------------------ */
/* Ring buffer — 44100 Hz mono int16 PCM stored as raw bytes          */
/* 16384 bytes = 8192 samples ≈ 186 ms; allocated from PSRAM          */
/* ------------------------------------------------------------------ */
#define A2DP_RING_BYTES  16384u
#define A2DP_RING_MASK   (A2DP_RING_BYTES - 1u)
static uint8_t     *s_ring = NULL;
static uint16_t     s_ring_wr = 0;
static uint16_t     s_ring_rd = 0;
static portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;

/* ------------------------------------------------------------------ */
/* Resampler state — 8 kHz mono → 44100 Hz mono                       */
/* Phase accumulator Q16; increment = 8000/44100 × 65536 ≈ 11891     */
/* ------------------------------------------------------------------ */
#define RESAMPLE_PHASE_INC  11891u

/* 240 input samples (30 ms @ 8 kHz) → max ≈ ceil(240×441/80)+1 = 1325 */
#define RESAMPLE_BUF_SAMPLES  1330
static int16_t *s_resample_buf = NULL;
static uint32_t s_resample_phase = 0;
static int16_t  s_resample_prev  = 0;

/* ------------------------------------------------------------------ */
/* Retry / reconnect timer                                             */
/* ------------------------------------------------------------------ */
static esp_timer_handle_t s_retry_timer = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool name_matches(const char *name)
{
    if (s_target_name[0] == '\0') {
        return true;  /* empty target = connect to first A2DP device found */
    }
    return strcmp(name, s_target_name) == 0;
}

static void start_scan(void)
{
    s_state = A2DP_STATE_SCANNING;
    ESP_LOGI(TAG, "Scanning for A2DP device '%s'...",
             s_target_name[0] ? s_target_name : "<any>");
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                               bt_scan_inquiry_len(), 0);
}

static void try_connect(const esp_bd_addr_t bda)
{
    memcpy(s_peer_bda, bda, sizeof(esp_bd_addr_t));
    s_state = A2DP_STATE_CONNECTING;
    bt_scan_set_link(s_peer_bda, BT_SCAN_LINK_CONNECTING);
    ESP_LOGI(TAG, "Target found, connecting A2DP...");
    esp_a2d_source_connect(s_peer_bda);
}

/* ------------------------------------------------------------------ */
/* Retry timer callback — behaviour depends on current state          */
/* ------------------------------------------------------------------ */
static void retry_timer_cb(void *arg)
{
    (void)arg;
    switch (s_state) {
    case A2DP_STATE_DISCONNECTED:
        start_scan();
        break;
    case A2DP_STATE_CONNECTING:
        /* Retry media readiness check after previous failure */
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* GAP callback — device discovery                                    */
/* ------------------------------------------------------------------ */
static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {

    case ESP_BT_GAP_DISC_RES_EVT: {
        char name[64] = {0};
        bool found = bt_scan_on_disc_res(param, name, sizeof(name));
        if (s_state != A2DP_STATE_SCANNING) break;
        if (found && name_matches(name)) {
            ESP_LOGI(TAG, "Found target: %s", name);
            esp_bt_gap_cancel_discovery();
            try_connect(param->disc_res.bda);
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        bt_scan_on_disc_state(param);
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            if (s_state == A2DP_STATE_SCANNING) {
                /* Scan ended without finding target — retry after 5 s */
                s_state = A2DP_STATE_DISCONNECTED;
                ESP_LOGW(TAG, "Target '%s' not found, retry in 5 s",
                         s_target_name[0] ? s_target_name : "<any>");
                esp_timer_start_once(s_retry_timer, 5000000ULL);
            }
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            bt_scan_on_auth_cmpl(param->auth_cmpl.bda,
                                 (const char *)param->auth_cmpl.device_name);
        }
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

/* ------------------------------------------------------------------ */
/* A2DP callback — connection and media control                       */
/* ------------------------------------------------------------------ */
static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {

    case ESP_A2D_CONNECTION_STATE_EVT: {
        esp_a2d_connection_state_t st = param->conn_stat.state;
        ESP_LOGI(TAG, "A2DP connection state: %d", st);
        if (st == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            bt_scan_set_link(s_peer_bda, BT_SCAN_LINK_CONNECTING);
        } else if (st == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            /* A2DP L2CAP connected; now wait for media readiness */
            s_state = A2DP_STATE_CONNECTING;
            memcpy(s_peer_bda, param->conn_stat.remote_bda, sizeof(esp_bd_addr_t));
            bt_scan_set_link(s_peer_bda, BT_SCAN_LINK_CONNECTING);
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        } else if (st == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            bool was_connected = (s_state == A2DP_STATE_CONNECTED);
            s_state = A2DP_STATE_DISCONNECTED;
            bt_scan_set_link(param->conn_stat.remote_bda, BT_SCAN_LINK_NONE);
            ESP_LOGW(TAG, "A2DP disconnected");
            if (s_callbacks.on_connection_state) {
                s_callbacks.on_connection_state(A2DP_STATE_DISCONNECTED);
            }
            if (was_connected) {
                /* Give speaker 5 s to settle before rescanning */
                esp_timer_start_once(s_retry_timer, 5000000ULL);
            }
        }
        break;
    }

    case ESP_A2D_MEDIA_CTRL_ACK_EVT: {
        esp_a2d_media_ctrl_t     cmd = param->media_ctrl_stat.cmd;
        esp_a2d_media_ctrl_ack_t ack = param->media_ctrl_stat.status;
        if (cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) {
            if (ack == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            } else {
                /* Not ready yet — retry in 500 ms (state stays CONNECTING) */
                esp_timer_start_once(s_retry_timer, 500000ULL);
            }
        } else if (cmd == ESP_A2D_MEDIA_CTRL_START) {
            if (ack == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                /* Reset resampler and ring buffer for fresh stream */
                s_resample_phase = 0;
                s_resample_prev  = 0;
                portENTER_CRITICAL(&s_ring_mux);
                s_ring_wr = 0;
                s_ring_rd = 0;
                portEXIT_CRITICAL(&s_ring_mux);
                s_state = A2DP_STATE_CONNECTED;
                bt_scan_set_link(s_peer_bda, BT_SCAN_LINK_CONNECTED);
                ESP_LOGI(TAG, "A2DP media started");
                if (s_callbacks.on_connection_state) {
                    s_callbacks.on_connection_state(A2DP_STATE_CONNECTED);
                }
            } else {
                /* START failed — retry media handshake after 1 s */
                ESP_LOGW(TAG, "A2DP media start failed, retrying");
                esp_timer_start_once(s_retry_timer, 1000000ULL);
            }
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %d", param->audio_stat.state);
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND &&
             s_state == A2DP_STATE_CONNECTED) {
            s_state = A2DP_STATE_CONNECTING;
            esp_timer_start_once(s_retry_timer, 200000ULL);
        }
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        ESP_LOGI(TAG, "A2DP SBC negotiated (type=%d)", param->audio_cfg.mcc.type);
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* A2DP data callback — drains ring, expands mono → stereo            */
/* ------------------------------------------------------------------ */
static int32_t a2dp_data_cb(uint8_t *data, int32_t len)
{
    /*
     * BT stack requests `len` bytes of 44100 Hz stereo int16 PCM.
     * One stereo sample pair = 4 bytes (L int16 + R int16).
     * Ring holds 44100 Hz mono int16 (2 bytes each).
     * Drain stereo_pairs × 2 bytes of mono, then expand to stereo.
     */
    int32_t  stereo_pairs = len / 4;
    int16_t *out          = (int16_t *)data;

    portENTER_CRITICAL(&s_ring_mux);
    uint16_t avail  = (uint16_t)((s_ring_wr - s_ring_rd) & A2DP_RING_MASK);
    uint16_t needed = (uint16_t)(stereo_pairs * 2);
    uint16_t drain  = (avail < needed) ? avail : needed;
    drain &= ~1u;  /* keep int16-aligned */

    if (drain > 0) {
        uint16_t to_end = A2DP_RING_BYTES - s_ring_rd;
        if (drain <= to_end) {
            memcpy(data, s_ring + s_ring_rd, drain);
        } else {
            memcpy(data, s_ring + s_ring_rd, to_end);
            memcpy(data + to_end, s_ring, drain - to_end);
        }
        s_ring_rd = (s_ring_rd + drain) & A2DP_RING_MASK;
    }
    portEXIT_CRITICAL(&s_ring_mux);

    /*
     * Expand mono int16 → stereo int16 in-place, iterating backwards so
     * later mono samples are moved before earlier ones can be overwritten.
     * mono_src[i] is at byte 2i; out[2i] is at byte 4i — no aliasing
     * issues when going backwards.
     */
    uint16_t mono_count = drain / 2;
    for (int32_t i = stereo_pairs - 1; i >= 0; i--) {
        int16_t s = (i < (int32_t)mono_count) ? ((int16_t *)data)[i] : 0;
        out[i * 2]     = s;
        out[i * 2 + 1] = s;
    }

    return len;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

esp_err_t a2dp_mgr_init(const char *own_device_name,
                         const char *target_device_name,
                         const a2dp_callbacks_t *callbacks)
{
    if (own_device_name == NULL || callbacks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&s_callbacks, callbacks, sizeof(a2dp_callbacks_t));
    bt_scan_set_role(BT_MODE_A2DP);
    if (target_device_name) {
        strncpy(s_target_name, target_device_name, sizeof(s_target_name) - 1);
    }

    /* Allocate large audio buffers from PSRAM to keep DRAM BSS free */
    s_ring = heap_caps_malloc(A2DP_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_ring == NULL) {
        ESP_LOGE(TAG, "Failed to allocate A2DP ring buffer (%u bytes) from PSRAM",
                 A2DP_RING_BYTES);
        return ESP_ERR_NO_MEM;
    }
    s_resample_buf = heap_caps_malloc(RESAMPLE_BUF_SAMPLES * sizeof(int16_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_resample_buf == NULL) {
        heap_caps_free(s_ring);
        s_ring = NULL;
        ESP_LOGE(TAG, "Failed to allocate resample buffer from PSRAM");
        return ESP_ERR_NO_MEM;
    }

    /* BT controller + Bluedroid init (mirrors hfp_client_init) */
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* sdkconfig uses BTDM firmware — must enable with BTDM even in A2DP-only mode.
     * BLE remains idle; only Classic BT callbacks (A2DP) are registered. */
    ret = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /*
     * Disable BT modem sleep so the controller stays awake for A2DP isochronous
     * SCO-like timing windows (same reason as in HFP mode).
     */
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

    ret = esp_bt_gap_set_device_name(own_device_name);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Set device name failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_register_callback(gap_cb);

    ret = esp_a2d_source_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "A2DP source init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_a2d_register_callback(a2dp_cb);
    esp_a2d_source_register_data_callback(a2dp_data_cb);

    /* Create retry/reconnect timer (single-shot, restarted as needed) */
    esp_timer_create_args_t ta = {
        .callback              = retry_timer_cb,
        .name                  = "a2dp_retry",
        .dispatch_method       = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    ret = esp_timer_create(&ta, &s_retry_timer);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create retry timer: %s", esp_err_to_name(ret));
        return ret;
    }

    start_scan();
    return ESP_OK;
}

void a2dp_push_audio(const int16_t *pcm_8k_mono, size_t sample_count)
{
    if (s_state != A2DP_STATE_CONNECTED || pcm_8k_mono == NULL || sample_count == 0) {
        return;
    }
    if (sample_count > 240) {
        sample_count = 240;
    }

    /*
     * Resample 8 kHz → 44100 Hz using linear interpolation with a Q16
     * phase accumulator.  RESAMPLE_PHASE_INC ≈ 8000/44100 × 65536 = 11891,
     * so each output sample advances the phase by 8000/44100 of an input step.
     */
    size_t out_count = 0;
    for (size_t i = 0; i < sample_count; i++) {
        int16_t s0 = s_resample_prev;
        int16_t s1 = pcm_8k_mono[i];
        while (s_resample_phase < 65536u && out_count < RESAMPLE_BUF_SAMPLES) {
            uint32_t frac = s_resample_phase;
            int32_t val = ((int32_t)s0 * (int32_t)(65536u - frac) +
                           (int32_t)s1 * (int32_t)frac) >> 16;
            s_resample_buf[out_count++] = (int16_t)val;
            s_resample_phase += RESAMPLE_PHASE_INC;
        }
        s_resample_phase -= 65536u;
        s_resample_prev = pcm_8k_mono[i];
    }

    if (out_count == 0) {
        return;
    }

    /* Write resampled mono int16 samples into the ring buffer as raw bytes */
    size_t write_bytes = out_count * sizeof(int16_t);
    portENTER_CRITICAL(&s_ring_mux);
    uint16_t free_space = (A2DP_RING_BYTES - 1u) -
                          ((s_ring_wr - s_ring_rd) & A2DP_RING_MASK);
    if (write_bytes > free_space) {
        write_bytes = (size_t)(free_space & ~1u);  /* keep int16-aligned */
    }
    if (write_bytes > 0) {
        const uint8_t *src  = (const uint8_t *)s_resample_buf;
        uint16_t       to_end = A2DP_RING_BYTES - s_ring_wr;
        if ((uint16_t)write_bytes <= to_end) {
            memcpy(s_ring + s_ring_wr, src, write_bytes);
        } else {
            memcpy(s_ring + s_ring_wr, src, to_end);
            memcpy(s_ring, src + to_end, write_bytes - to_end);
        }
        s_ring_wr = (s_ring_wr + (uint16_t)write_bytes) & A2DP_RING_MASK;
    }
    portEXIT_CRITICAL(&s_ring_mux);
}

a2dp_conn_state_t a2dp_get_conn_state(void)
{
    return s_state;
}
