/**
 * Audio Bridge — Bluetooth SCO/A2DP ↔ Qwen Audio Agent PCM
 *
 * Capture: HFP PCM (8 kHz CVSD or 16 kHz mSBC) is resampled to the Gateway
 *          input rate and sent as 60 ms input_audio_buffer.append frames.
 *
 * Playout: audio.delta PCM (usually 24 kHz) is resampled to the SCO rate
 *          and paced into the HFP TX ring by remaining playout depth, the
 *          same way the original SIP gateway absorbed WiFi timer jitter.
 */

#include "audio_bridge.h"
#include "audio_codec.h"
#include "audio_coex.h"
#include "qwen_client.h"
#include "hfp_client_mgr.h"
#include "hfp_ag_mgr.h"
#include "a2dp_mgr.h"
#include "config_manager.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

static const char *TAG = "audio_bridge";

#define MAX_PCM_24K          2048   /* Covers a 4096-char relay audio chunk */
#define CAPTURE_FRAME_MS     60u
#define PLAY_FRAME_MS        20u
#define CAPTURE_RING_SAMPLES 32768u /* About 2 s at 16 kHz */
#define CAPTURE_RING_MASK    (CAPTURE_RING_SAMPLES - 1u)
#define CAPTURE_TX_BURST     4u
#define PLAY_RING_SAMPLES    262144u /* 16 s at 16 kHz; preserves normal-speed burst playback */
#define PLAY_RING_MASK       (PLAY_RING_SAMPLES - 1u)
#define PUMP_PERIOD_MS       10u
#define TX_BURST_MAX         4u
#define PLAY_FEED_BURST      6u
#define SCO_FLOOR_MS         8u
#define PLAY_PREBUFFER_MS   200u
#define PLAY_REBUFFER_US  150000LL
#define BARGE_VOICE_MS      180u
#define BARGE_AVG_LEVEL     700u
#define BARGE_PEAK_LEVEL   2500u
#define STATS_PERIOD_US     5000000LL
#define AUDIO_TASK_DONE_BIT (1u << 0)
#define CAPTURE_TASK_DONE_BIT (1u << 1)

static EXT_RAM_BSS_ATTR int16_t s_cap_tmp[MAX_PCM_24K];
static EXT_RAM_BSS_ATTR int16_t s_cap_ring[CAPTURE_RING_SAMPLES];
static EXT_RAM_BSS_ATTR int16_t s_cap_frame[MAX_PCM_24K];
static uint16_t s_cap_wr;
static uint16_t s_cap_rd;
static size_t s_cap_count;
static bool s_cap_overflow_logged;
static portMUX_TYPE s_cap_mux = portMUX_INITIALIZER_UNLOCKED;

static EXT_RAM_BSS_ATTR int16_t s_play_tmp[MAX_PCM_24K];
static EXT_RAM_BSS_ATTR int16_t s_play_ring[PLAY_RING_SAMPLES];
static uint32_t s_play_wr;
static uint32_t s_play_rd;
static bool s_play_overflow_logged;
static portMUX_TYPE s_play_mux = portMUX_INITIALIZER_UNLOCKED;

static char s_play_response_id[64];
static char s_cancelled_response_id[64];
static bool s_playback_started;
static bool s_playback_ending;
static bool s_playback_buffering;
static int64_t s_play_empty_since_us;
static uint32_t s_barge_voiced_samples;
static uint32_t s_barge_max_level;
static uint32_t s_mic_max_level;
static uint32_t s_mic_active_samples;
static volatile bool s_barge_clear_pending;

static esp_timer_handle_t s_pump_timer;
static TaskHandle_t s_audio_task;
static TaskHandle_t s_capture_task;
static EventGroupHandle_t s_task_done;
static EventBits_t s_task_expected;
static volatile bool s_audio_task_run;
static int64_t s_tx_due_us;
static QueueHandle_t s_receipts;
static uint32_t s_play_dropped;
static uint32_t s_capture_dropped;
static uint32_t s_capture_in_samples;
static uint32_t s_play_underflows;
static uint32_t s_prev_play_dropped;
static uint32_t s_prev_capture_dropped;
static uint32_t s_prev_capture_in_samples;
static uint32_t s_prev_play_underflows;
static uint32_t s_prev_sco_calls;
static uint32_t s_prev_sco_pad;
static uint32_t s_prev_sco_trim;
static int64_t s_stats_us;

typedef struct {
    char response_id[64];
    enum { RECEIPT_STARTED, RECEIPT_ENDED, RECEIPT_CANCELLED,
           RECEIPT_CANCELLED_USER } type;
    int64_t queued_us;
} playback_receipt_t;

static void queue_receipt(int type, const char *response_id)
{
    if (s_receipts == NULL || response_id == NULL || !response_id[0]) {
        return;
    }
    playback_receipt_t receipt = {.type = type, .queued_us = esp_timer_get_time()};
    snprintf(receipt.response_id, sizeof(receipt.response_id), "%s", response_id);
    if (xQueueSend(s_receipts, &receipt, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Playback receipt queue full type=%d response=%s", type, response_id);
    }
}

static void clear_playback(bool user_interruption);

typedef struct {
    bool                  active;
    audio_bridge_config_t config;
} audio_bridge_state_t;

static audio_bridge_state_t s_bridge;

static int sco_rate(void)
{
    return (s_bridge.config.sco_codec == SCO_CODEC_MSBC) ? 16000 : 8000;
}

static unsigned ptime_ms(void)
{
    return PLAY_FRAME_MS;
}

static unsigned sco_target_ms(void)
{
    return audio_path_on_wifi() ? 80u : 30u;
}

static int sco_level_ms(void)
{
    const system_config_t *cfg = config_get();
    if (cfg == NULL) {
        return hfp_sco_out_level_ms();
    }
    if (cfg->bt_mode == BT_MODE_A2DP) {
        return -1;
    }
    if (cfg->bt_mode == BT_MODE_HFP_AG) {
        return hfp_ag_sco_out_level_ms();
    }
    return hfp_sco_out_level_ms();
}

static unsigned frames_due(int64_t *due_us, int64_t now_us)
{
    int64_t period = (int64_t)ptime_ms() * 1000;
    if (*due_us == 0 || now_us - *due_us > 8 * period) {
        *due_us = now_us + period;
        return 1;
    }
    unsigned n = 0;
    while (now_us >= *due_us) {
        *due_us += period;
        n++;
    }
    return n;
}

static size_t play_avail(void)
{
    return (s_play_wr - s_play_rd) & PLAY_RING_MASK;
}

static size_t play_queued(void)
{
    portENTER_CRITICAL(&s_play_mux);
    size_t queued = play_avail();
    portEXIT_CRITICAL(&s_play_mux);
    return queued;
}

static void play_push(const int16_t *pcm, size_t n)
{
    portENTER_CRITICAL(&s_play_mux);
    size_t free_space = (PLAY_RING_SAMPLES - 1u) - play_avail();
    size_t write_n = n < free_space ? n : free_space;
    bool dropped = write_n < n;
    if (dropped) {
        s_play_dropped += n - write_n;
    }
    bool log_drop = dropped && !s_play_overflow_logged;
    if (log_drop) {
        s_play_overflow_logged = true;
    }
    size_t first = write_n < PLAY_RING_SAMPLES - s_play_wr
        ? write_n : PLAY_RING_SAMPLES - s_play_wr;
    memcpy(s_play_ring + s_play_wr, pcm, first * sizeof(int16_t));
    memcpy(s_play_ring, pcm + first, (write_n - first) * sizeof(int16_t));
    s_play_wr = (s_play_wr + write_n) & PLAY_RING_MASK;
    portEXIT_CRITICAL(&s_play_mux);
    if (log_drop) {
        ESP_LOGW(TAG, "Playback ring full; dropping incoming audio");
    }
}

static size_t play_pop(int16_t *out, size_t n)
{
    portENTER_CRITICAL(&s_play_mux);
    size_t avail = play_avail();
    size_t copy = n < avail ? n : avail;
    size_t first = copy < PLAY_RING_SAMPLES - s_play_rd
        ? copy : PLAY_RING_SAMPLES - s_play_rd;
    memcpy(out, s_play_ring + s_play_rd, first * sizeof(int16_t));
    memcpy(out + first, s_play_ring, (copy - first) * sizeof(int16_t));
    s_play_rd = (s_play_rd + copy) & PLAY_RING_MASK;
    portEXIT_CRITICAL(&s_play_mux);
    return copy;
}

static void push_sco(const int16_t *pcm, size_t samples)
{
    const uint8_t *bytes = (const uint8_t *)pcm;
    uint32_t len = (uint32_t)(samples * sizeof(int16_t));
    const system_config_t *cfg = config_get();
    if (cfg && cfg->bt_mode == BT_MODE_A2DP) {
        /* A2DP source expects 8 kHz mono. */
        if (sco_rate() == 8000) {
            a2dp_push_audio(pcm, samples);
        } else {
            size_t n8 = resample_linear(pcm, samples, sco_rate(),
                                        s_play_tmp, MAX_PCM_24K, 8000);
            a2dp_push_audio(s_play_tmp, n8);
        }
        return;
    }
    if (cfg && cfg->bt_mode == BT_MODE_HFP_AG) {
        hfp_ag_push_audio(bytes, len);
    } else {
        hfp_push_audio(bytes, len);
    }
}

static void maybe_finish_playback(void)
{
    if (!s_playback_ending) {
        return;
    }
    if (play_queued() > 0) {
        return;
    }
    if (s_playback_started && s_play_response_id[0]) {
        queue_receipt(RECEIPT_ENDED, s_play_response_id);
    }
    s_playback_started = false;
    s_playback_ending = false;
    s_playback_buffering = false;
    s_play_empty_since_us = 0;
    s_play_response_id[0] = '\0';
}

static void feed_playout(int64_t now_us)
{
    int rate = sco_rate();
    size_t frame = (size_t)rate * ptime_ms() / 1000u;
    if (frame == 0 || frame > MAX_PCM_24K) {
        return;
    }

    if (s_playback_buffering) {
        size_t queued = play_queued();
        size_t threshold = (size_t)rate * PLAY_PREBUFFER_MS / 1000u;
        if (queued == 0) {
            maybe_finish_playback();
            return;
        }
        if (queued < threshold && !s_playback_ending) {
            return;
        }
        s_playback_buffering = false;
        s_play_empty_since_us = 0;
    }

    int level = sco_level_ms();
    unsigned budget;
    if (level >= 0) {
        unsigned target = sco_target_ms();
        if ((unsigned)level >= target) {
            return;
        }
        budget = (target - (unsigned)level + ptime_ms() - 1u) / ptime_ms();
    } else {
        budget = frames_due(&s_tx_due_us, now_us);
    }
    if (budget > PLAY_FEED_BURST) {
        budget = PLAY_FEED_BURST;
    }

    static EXT_RAM_BSS_ATTR int16_t s_frame[MAX_PCM_24K];
    while (budget-- > 0) {
        size_t got = play_pop(s_frame, frame);
        if (got == 0) {
            if (s_playback_started && !s_playback_ending && level <= SCO_FLOOR_MS) {
                s_play_underflows++;
                if (s_play_empty_since_us == 0) {
                    s_play_empty_since_us = now_us;
                } else if (now_us - s_play_empty_since_us >= PLAY_REBUFFER_US) {
                    s_playback_buffering = true;
                }
            }
            maybe_finish_playback();
            break;
        }
        s_play_empty_since_us = 0;
        if (got < frame) {
            memset(s_frame + got, 0, (frame - got) * sizeof(int16_t));
        }
        if (!s_playback_started && s_play_response_id[0]) {
            queue_receipt(RECEIPT_STARTED, s_play_response_id);
            s_playback_started = true;
            ESP_LOGI(TAG, "Playback started response=%s queued=%ums",
                     s_play_response_id,
                     (unsigned)(play_queued() * 1000u / (unsigned)rate));
        }
        push_sco(s_frame, frame);
    }
}

static void flush_capture(void)
{
    int rate = qwen_client_input_sample_rate();
    if (rate <= 0) {
        rate = s_bridge.config.input_rate > 0 ? s_bridge.config.input_rate : 16000;
    }
    size_t frame = (size_t)rate * CAPTURE_FRAME_MS / 1000u;
    if (frame == 0 || frame > MAX_PCM_24K) {
        return;
    }

    for (unsigned burst = 0; burst < CAPTURE_TX_BURST && s_audio_task_run; burst++) {
        portENTER_CRITICAL(&s_cap_mux);
        if (s_cap_count < frame) {
            portEXIT_CRITICAL(&s_cap_mux);
            break;
        }
        size_t first = frame < CAPTURE_RING_SAMPLES - s_cap_rd
            ? frame : CAPTURE_RING_SAMPLES - s_cap_rd;
        memcpy(s_cap_frame, s_cap_ring + s_cap_rd, first * sizeof(int16_t));
        memcpy(s_cap_frame + first, s_cap_ring, (frame - first) * sizeof(int16_t));
        s_cap_rd = (uint16_t)((s_cap_rd + frame) & CAPTURE_RING_MASK);
        s_cap_count -= frame;
        portEXIT_CRITICAL(&s_cap_mux);
        if (qwen_client_send_pcm(s_cap_frame, frame) != ESP_OK) {
            break;
        }
    }
}

static void capture_tx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Capture WS task started");
    while (s_audio_task_run) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CAPTURE_FRAME_MS));
        if (!s_audio_task_run) {
            break;
        }
        playback_receipt_t receipt;
        while (s_audio_task_run && s_receipts &&
               xQueueReceive(s_receipts, &receipt, 0) == pdTRUE) {
            ESP_LOGI(TAG, "Playback receipt type=%d response=%s queued=%lldms",
                     receipt.type, receipt.response_id,
                     (long long)((esp_timer_get_time() - receipt.queued_us) / 1000));
            if (receipt.type == RECEIPT_STARTED) {
                qwen_client_send_playback_started(receipt.response_id);
            } else if (receipt.type == RECEIPT_ENDED) {
                qwen_client_send_playback_ended(receipt.response_id);
            } else if (receipt.type == RECEIPT_CANCELLED_USER) {
                qwen_client_send_playback_cancelled_reason(receipt.response_id,
                                                           "user_interruption");
            } else {
                qwen_client_send_playback_cancelled(receipt.response_id);
            }
        }
        if (s_bridge.active) {
            flush_capture();
        }
    }
    ESP_LOGI(TAG, "Capture WS task exiting");
    s_capture_task = NULL;
    xEventGroupSetBits(s_task_done, CAPTURE_TASK_DONE_BIT);
    vTaskDelete(NULL);
}

static void stats_report(int64_t now_us)
{
    if (s_stats_us == 0) {
        s_stats_us = now_us;
        return;
    }
    if (now_us - s_stats_us < STATS_PERIOD_US) {
        return;
    }
    portENTER_CRITICAL(&s_play_mux);
    unsigned play_ms = (unsigned)play_avail() * 1000u / (unsigned)sco_rate();
    uint32_t play_dropped = s_play_dropped;
    portEXIT_CRITICAL(&s_play_mux);
    portENTER_CRITICAL(&s_cap_mux);
    unsigned cap_ms = (unsigned)s_cap_count * 1000u /
        (unsigned)qwen_client_input_sample_rate();
    uint32_t cap_dropped = s_capture_dropped;
    uint32_t cap_in_samples = s_capture_in_samples;
    portEXIT_CRITICAL(&s_cap_mux);
    uint32_t underflows = s_play_underflows;
    uint32_t barge_max_level = __atomic_exchange_n(&s_barge_max_level, 0,
                                                   __ATOMIC_RELAXED);
    uint32_t mic_max_level = __atomic_exchange_n(&s_mic_max_level, 0,
                                                 __ATOMIC_RELAXED);
    uint32_t mic_active_samples = __atomic_exchange_n(&s_mic_active_samples, 0,
                                                       __ATOMIC_RELAXED);
    uint32_t calls = 0, pad = 0, trims = 0;
    const system_config_t *cfg = config_get();
    if (cfg && cfg->bt_mode == BT_MODE_HFP_AG) {
        hfp_ag_sco_tx_stats(&calls, &pad, &trims);
    } else if (!cfg || cfg->bt_mode == BT_MODE_HFP) {
        hfp_sco_tx_stats(&calls, &pad, &trims);
    }
    ESP_LOGI(TAG, "Audio 5s: sco=%dms play=%ums cap=%ums active=%d buffering=%d play_drop=%lu cap_drop=%lu play_empty=%lu mic_peak=%lu mic_voice=%lums barge_level=%lu sco_cb=%lu pad=%lu trim=%lu",
             sco_level_ms(), play_ms, cap_ms, (int)s_playback_started,
             (int)s_playback_buffering,
             (unsigned long)(play_dropped - s_prev_play_dropped),
             (unsigned long)(cap_dropped - s_prev_capture_dropped),
             (unsigned long)(underflows - s_prev_play_underflows),
             (unsigned long)mic_max_level,
             (unsigned long)(mic_active_samples * 1000u / (unsigned)sco_rate()),
             (unsigned long)barge_max_level,
             (unsigned long)(calls - s_prev_sco_calls),
             (unsigned long)(pad - s_prev_sco_pad),
             (unsigned long)(trims - s_prev_sco_trim));
    qwen_pcm_tx_stats_t tx;
    qwen_client_take_pcm_tx_stats(&tx);
    unsigned input_rate = (unsigned)qwen_client_input_sample_rate();
    ESP_LOGI(TAG, "WS TX 5s: in=%lums sent=%lums frames=%lu avg=%lums max=%lums failed=%lu",
             (unsigned long)((uint64_t)(cap_in_samples - s_prev_capture_in_samples)
                             * 1000u / input_rate),
             (unsigned long)((uint64_t)tx.samples * 1000u / input_rate),
             (unsigned long)tx.frames,
             (unsigned long)(tx.frames ? tx.spent_ms / tx.frames : 0),
             (unsigned long)tx.max_ms, (unsigned long)tx.failures);
    s_prev_play_dropped = play_dropped;
    s_prev_capture_dropped = cap_dropped;
    s_prev_capture_in_samples = cap_in_samples;
    s_prev_play_underflows = underflows;
    s_prev_sco_calls = calls;
    s_prev_sco_pad = pad;
    s_prev_sco_trim = trims;
    s_stats_us = now_us;
}

static void audio_tx_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Audio TX task started");
    while (s_audio_task_run) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!s_audio_task_run) {
            break;
        }
        if (!s_bridge.active) {
            s_tx_due_us = 0;
            continue;
        }
        if (__atomic_exchange_n(&s_barge_clear_pending, false, __ATOMIC_ACQ_REL)) {
            if (s_playback_started && s_play_response_id[0]) {
                snprintf(s_cancelled_response_id, sizeof(s_cancelled_response_id),
                         "%s", s_play_response_id);
                ESP_LOGI(TAG, "Local barge-in: cancelling response %s", s_cancelled_response_id);
                qwen_client_discard_response(s_cancelled_response_id);
                clear_playback(true);
                if (s_capture_task) {
                    xTaskNotifyGive(s_capture_task);
                }
            }
        }
        int64_t now_us = esp_timer_get_time();
        feed_playout(now_us);
        stats_report(now_us);
    }
    ESP_LOGI(TAG, "Audio TX task exiting");
    s_audio_task = NULL;
    xEventGroupSetBits(s_task_done, AUDIO_TASK_DONE_BIT);
    vTaskDelete(NULL);
}

static void pump_timer_cb(void *arg)
{
    (void)arg;
    if (!s_bridge.active || s_audio_task == NULL) {
        return;
    }
    xTaskNotifyGive(s_audio_task);
}

esp_err_t audio_bridge_start(const audio_bridge_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_bridge.active || s_audio_task != NULL || s_capture_task != NULL ||
        s_receipts != NULL || s_task_done != NULL) {
        esp_err_t stop_err = audio_bridge_stop();
        if (stop_err != ESP_OK) {
            return stop_err;
        }
    }

    memcpy(&s_bridge.config, config, sizeof(*config));
    s_cap_wr = 0;
    s_cap_rd = 0;
    s_cap_count = 0;
    s_cap_overflow_logged = false;
    s_play_wr = 0;
    s_play_rd = 0;
    s_play_overflow_logged = false;
    s_tx_due_us = 0;
    s_playback_started = false;
    s_playback_ending = false;
    s_playback_buffering = false;
    s_play_empty_since_us = 0;
    s_play_response_id[0] = '\0';
    s_cancelled_response_id[0] = '\0';
    s_barge_voiced_samples = 0;
    s_barge_max_level = 0;
    s_mic_max_level = 0;
    s_mic_active_samples = 0;
    s_barge_clear_pending = false;
    s_play_dropped = 0;
    s_capture_dropped = 0;
    s_capture_in_samples = 0;
    s_play_underflows = 0;
    s_prev_play_dropped = 0;
    s_prev_capture_dropped = 0;
    s_prev_capture_in_samples = 0;
    s_prev_play_underflows = 0;
    const system_config_t *cfg = config_get();
    if (cfg && cfg->bt_mode == BT_MODE_HFP_AG) {
        hfp_ag_sco_tx_stats(&s_prev_sco_calls, &s_prev_sco_pad, &s_prev_sco_trim);
    } else if (!cfg || cfg->bt_mode == BT_MODE_HFP) {
        hfp_sco_tx_stats(&s_prev_sco_calls, &s_prev_sco_pad, &s_prev_sco_trim);
    } else {
        s_prev_sco_calls = s_prev_sco_pad = s_prev_sco_trim = 0;
    }
    s_stats_us = 0;
    qwen_pcm_tx_stats_t old_tx;
    qwen_client_take_pcm_tx_stats(&old_tx);
    codec_resampler_reset();

    s_task_done = xEventGroupCreate();
    if (s_task_done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_receipts = xQueueCreate(8, sizeof(playback_receipt_t));
    if (s_receipts == NULL) {
        vEventGroupDelete(s_task_done);
        s_task_done = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_audio_task_run = true;
    s_task_expected = 0;
    BaseType_t ok = xTaskCreatePinnedToCore(
        audio_tx_task, "audio_tx", 4096, NULL, 15, &s_audio_task, 1);
    if (ok != pdPASS) {
        s_audio_task_run = false;
        s_audio_task = NULL;
        vQueueDelete(s_receipts);
        s_receipts = NULL;
        vEventGroupDelete(s_task_done);
        s_task_done = NULL;
        ESP_LOGE(TAG, "Failed to create audio TX task");
        return ESP_ERR_NO_MEM;
    }
    s_task_expected |= AUDIO_TASK_DONE_BIT;
    ok = xTaskCreatePinnedToCore(
        capture_tx_task, "capture_ws", 4096, NULL, 10, &s_capture_task, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create capture WS task");
        audio_bridge_stop();
        return ESP_ERR_NO_MEM;
    }
    s_task_expected |= CAPTURE_TASK_DONE_BIT;

    esp_timer_create_args_t ta = {
        .callback = pump_timer_cb,
        .name = "qwen_pump",
        .dispatch_method = ESP_TIMER_TASK,
        .skip_unhandled_events = true,
    };
    esp_err_t ret = esp_timer_create(&ta, &s_pump_timer);
    if (ret != ESP_OK) {
        audio_bridge_stop();
        return ret;
    }
    ret = esp_timer_start_periodic(s_pump_timer, (uint64_t)PUMP_PERIOD_MS * 1000ULL);
    if (ret != ESP_OK) {
        esp_timer_delete(s_pump_timer);
        s_pump_timer = NULL;
        audio_bridge_stop();
        return ret;
    }

    s_bridge.active = true;
    ESP_LOGI(TAG, "Audio bridge started: sco=%d Hz, input=%d Hz, capture=%d",
             sco_rate(), s_bridge.config.input_rate, (int)s_bridge.config.capture);
    return ESP_OK;
}

esp_err_t audio_bridge_stop(void)
{
    if (!s_bridge.active && s_audio_task == NULL && s_capture_task == NULL &&
        s_receipts == NULL && s_task_done == NULL) {
        return ESP_OK;
    }
    s_bridge.active = false;
    if (s_pump_timer != NULL) {
        esp_timer_stop(s_pump_timer);
        esp_timer_delete(s_pump_timer);
        s_pump_timer = NULL;
    }
    s_audio_task_run = false;
    if (s_task_expected != 0) {
        EventBits_t pending = s_task_expected;
        EventBits_t done = xEventGroupWaitBits(s_task_done, pending, pdFALSE,
                                               pdTRUE, pdMS_TO_TICKS(3500));
        if ((done & pending) != pending) {
            ESP_LOGE(TAG, "Audio tasks did not stop in time (pending=0x%lx done=0x%lx)",
                     (unsigned long)pending, (unsigned long)done);
            return ESP_ERR_TIMEOUT;
        }
        s_audio_task = NULL;
        s_capture_task = NULL;
        s_task_expected = 0;
    }
    if (s_receipts != NULL) {
        vQueueDelete(s_receipts);
        s_receipts = NULL;
    }
    if (s_task_done != NULL) {
        vTaskDelay(1);
        vEventGroupDelete(s_task_done);
        s_task_done = NULL;
    }
    if (s_playback_started && s_play_response_id[0]) {
        qwen_client_send_playback_cancelled(s_play_response_id);
    }
    s_playback_started = false;
    s_playback_ending = false;
    s_playback_buffering = false;
    s_play_empty_since_us = 0;
    s_play_response_id[0] = '\0';
    ESP_LOGI(TAG, "Audio bridge stopped");
    return ESP_OK;
}

bool audio_bridge_is_active(void)
{
    return s_bridge.active;
}

void audio_bridge_bt_to_qwen(const uint8_t *pcm_data, uint32_t len)
{
    if (!s_bridge.active || !s_bridge.config.capture || pcm_data == NULL || len < 2) {
        return;
    }

    const int16_t *pcm = (const int16_t *)pcm_data;
    size_t samples = len / sizeof(int16_t);
    int from = sco_rate();
    uint64_t total = 0;
    uint32_t peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int32_t value = pcm[i];
        uint32_t magnitude = (uint32_t)(value < 0 ? -value : value);
        total += magnitude;
        if (magnitude > peak) peak = magnitude;
    }
    uint32_t level = (uint32_t)(total / samples);
    uint32_t mic_max_level = __atomic_load_n(&s_mic_max_level, __ATOMIC_RELAXED);
    while (level > mic_max_level &&
           !__atomic_compare_exchange_n(&s_mic_max_level, &mic_max_level, level,
                                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (level >= BARGE_AVG_LEVEL && peak >= BARGE_PEAK_LEVEL) {
        __atomic_add_fetch(&s_mic_active_samples, (uint32_t)samples, __ATOMIC_RELAXED);
    }
    if (s_playback_started && !s_barge_clear_pending &&
        s_cancelled_response_id[0] == '\0') {
        uint32_t max_level = __atomic_load_n(&s_barge_max_level, __ATOMIC_RELAXED);
        while (level > max_level &&
               !__atomic_compare_exchange_n(&s_barge_max_level, &max_level, level,
                                            false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
        if (level >= BARGE_AVG_LEVEL && peak >= BARGE_PEAK_LEVEL) {
            s_barge_voiced_samples += samples;
        } else {
            uint32_t decay = (uint32_t)(samples * 2u);
            s_barge_voiced_samples = s_barge_voiced_samples > decay
                ? s_barge_voiced_samples - decay : 0;
        }
        if (s_barge_voiced_samples >= (uint32_t)from * BARGE_VOICE_MS / 1000u) {
            s_barge_voiced_samples = 0;
            __atomic_store_n(&s_barge_clear_pending, true, __ATOMIC_RELEASE);
        }
    } else if (!s_playback_started) {
        s_barge_voiced_samples = 0;
    }
    int to = qwen_client_input_sample_rate();
    if (to <= 0) {
        to = s_bridge.config.input_rate > 0 ? s_bridge.config.input_rate : 16000;
    }

    size_t converted = resample_linear(pcm, samples, from, s_cap_tmp, MAX_PCM_24K, to);
    if (converted == 0) {
        return;
    }
    bool dropped = false;
    portENTER_CRITICAL(&s_cap_mux);
    size_t room = CAPTURE_RING_SAMPLES - s_cap_count;
    if (converted > room) {
        size_t excess = converted - room;
        s_cap_rd = (uint16_t)((s_cap_rd + excess) & CAPTURE_RING_MASK);
        s_cap_count -= excess;
        s_capture_dropped += excess;
        dropped = !s_cap_overflow_logged;
        s_cap_overflow_logged = true;
    }
    size_t first = converted < CAPTURE_RING_SAMPLES - s_cap_wr
        ? converted : CAPTURE_RING_SAMPLES - s_cap_wr;
    memcpy(s_cap_ring + s_cap_wr, s_cap_tmp, first * sizeof(int16_t));
    memcpy(s_cap_ring, s_cap_tmp + first, (converted - first) * sizeof(int16_t));
    s_cap_wr = (uint16_t)((s_cap_wr + converted) & CAPTURE_RING_MASK);
    s_cap_count += converted;
    s_capture_in_samples += converted;
    portEXIT_CRITICAL(&s_cap_mux);
    if (dropped) {
        ESP_LOGW(TAG, "Capture ring full; dropping oldest audio");
    }
}

void audio_bridge_qwen_to_bt(const int16_t *pcm, size_t samples, int sample_rate,
                             const char *response_id)
{
    if (!s_bridge.active || pcm == NULL || samples == 0) {
        return;
    }
    if (sample_rate <= 0) {
        sample_rate = 24000;
    }
    if (response_id && response_id[0]) {
        if (strcmp(s_cancelled_response_id, response_id) == 0) {
            return;
        }
        if (strcmp(s_play_response_id, response_id) != 0) {
            if (s_playback_started && s_play_response_id[0]) {
                queue_receipt(RECEIPT_ENDED, s_play_response_id);
            }
            s_playback_started = false;
            s_playback_buffering = true;
            s_play_empty_since_us = 0;
            s_cancelled_response_id[0] = '\0';
        }
        strncpy(s_play_response_id, response_id, sizeof(s_play_response_id) - 1);
        s_play_response_id[sizeof(s_play_response_id) - 1] = '\0';
        s_playback_ending = false;
    } else if (!s_playback_started && play_queued() == 0) {
        s_playback_buffering = true;
    }

    size_t converted = resample_linear(pcm, samples, sample_rate,
                                       s_play_tmp, MAX_PCM_24K, sco_rate());
    if (converted > 0) {
        play_push(s_play_tmp, converted);
    }
}

void audio_bridge_on_audio_done(const char *response_id)
{
    if (!s_play_response_id[0] ||
        (response_id && response_id[0] && strcmp(response_id, s_play_response_id) != 0)) {
        return;
    }
    s_playback_ending = true;
}

static void clear_playback(bool user_interruption)
{
    portENTER_CRITICAL(&s_play_mux);
    unsigned queued_ms = (unsigned)play_avail() * 1000u / (unsigned)sco_rate();
    s_play_wr = 0;
    s_play_rd = 0;
    portEXIT_CRITICAL(&s_play_mux);
    portENTER_CRITICAL(&s_cap_mux);
    unsigned capture_ms = (unsigned)s_cap_count * 1000u /
        (unsigned)qwen_client_input_sample_rate();
    portEXIT_CRITICAL(&s_cap_mux);
    ESP_LOGI(TAG, "Playback clear: queued=%ums sco=%dms capture=%ums",
             queued_ms, sco_level_ms(), capture_ms);
    if (s_playback_started && s_play_response_id[0]) {
        queue_receipt(user_interruption ? RECEIPT_CANCELLED_USER : RECEIPT_CANCELLED,
                      s_play_response_id);
    }
    s_playback_started = false;
    s_playback_ending = false;
    s_playback_buffering = false;
    s_play_empty_since_us = 0;
    s_play_response_id[0] = '\0';
}

void audio_bridge_clear_playback(void)
{
    clear_playback(false);
}

void audio_bridge_set_sco_codec(sco_codec_t sco_codec)
{
    sco_codec_t prev = s_bridge.config.sco_codec;
    s_bridge.config.sco_codec = sco_codec;
    if (s_bridge.active && prev != sco_codec) {
        ESP_LOGI(TAG, "SCO codec changed: %d → %d", prev, sco_codec);
        codec_resampler_reset();
    }
}
