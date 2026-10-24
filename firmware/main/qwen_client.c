/**
 * Qwen Audio Agent Gateway Client (GCP 6.0 / wire 7.0.0)
 *
 * Speaks the Gateway Client Protocol over WebSocket:
 *   session.hello → session.ready → voice.ready
 *   input_audio_buffer.append  (PCM16LE mono, base64)
 *   audio.delta / audio.done
 *   playback.started|ended|cancelled
 *   session.ping / session.pong
 */

#include "qwen_client.h"
#include "config_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "qwen_cli";

#define QWEN_PROTOCOL_VERSION   "7.0.0"
#define QWEN_CLIENT_VERSION     "0.1.0"
#define QWEN_TX_JSON_MAX        4096
#define QWEN_TX_PCM_MAX         1440    /* 60 ms @ 24 kHz, 16-bit */
#define QWEN_RX_PCM_MAX         2048    /* audio_bridge's maximum input chunk */
#define QWEN_RX_JSON_INITIAL    16384
#define QWEN_RX_JSON_MAX        (640u * 1024u) /* 512 KB audio plus JSON envelope */
#define QWEN_RX_B64_CHARS       4096u   /* 1536 PCM16 samples per decode */
#define QWEN_AUDIO_QUEUE_DEPTH  8u
#define QWEN_B64_MAX            4096
#define QWEN_RESPONSE_ID_MAX    64
#define QWEN_CONTROL_JSON_MAX   320
/* A send timeout aborts the WebSocket, so allow brief TCP backpressure. */
#define QWEN_SEND_TIMEOUT_MS     2000

static esp_websocket_client_handle_t s_ws;
static qwen_client_callbacks_t s_cb;
static qwen_state_t s_state = QWEN_STATE_IDLE;
static uint32_t s_evt;
static int s_input_rate = 16000;
static bool s_voice_owned = true;
static bool s_capture_ok = true;
static char s_auth_header[160];
static char s_instance_id[20];
static char *s_tx_json;
static char *s_b64;
static int16_t *s_rx_pcm;
static char *s_rx_json;
static size_t s_rx_json_len;
static size_t s_rx_json_capacity;
static size_t s_rx_frame_offset;
static bool s_rx_collecting;
static bool s_rx_discard;
static uint32_t s_pcm_tx_frames;
static uint32_t s_pcm_tx_samples;
static uint32_t s_pcm_tx_spent_ms;
static uint32_t s_pcm_tx_max_ms;
static uint32_t s_pcm_tx_failures;
static bool s_logged_large_audio;
static char s_last_audio_response_id[QWEN_RESPONSE_ID_MAX];
static QueueHandle_t s_audio_queue;
static uint32_t s_audio_generation;
static portMUX_TYPE s_discard_mux = portMUX_INITIALIZER_UNLOCKED;
// Keep recent cancelled IDs so late audio frames cannot re-enter the decode queue.
static char s_discarded_responses[4][QWEN_RESPONSE_ID_MAX];
static unsigned s_discard_next;

typedef struct {
    char *json;
    int len;
    uint32_t generation;
    bool audio;
} queued_audio_t;

void qwen_client_take_pcm_tx_stats(qwen_pcm_tx_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    stats->frames = __atomic_exchange_n(&s_pcm_tx_frames, 0, __ATOMIC_RELAXED);
    stats->samples = __atomic_exchange_n(&s_pcm_tx_samples, 0, __ATOMIC_RELAXED);
    stats->spent_ms = __atomic_exchange_n(&s_pcm_tx_spent_ms, 0, __ATOMIC_RELAXED);
    stats->max_ms = __atomic_exchange_n(&s_pcm_tx_max_ms, 0, __ATOMIC_RELAXED);
    stats->failures = __atomic_exchange_n(&s_pcm_tx_failures, 0, __ATOMIC_RELAXED);
}

static uint32_t next_evt(void)
{
    return __atomic_add_fetch(&s_evt, 1, __ATOMIC_RELAXED);
}

static void set_state(qwen_state_t st)
{
    s_state = st;
}

static bool ws_open(void)
{
    return s_ws && esp_websocket_client_is_connected(s_ws);
}

static bool send_json(const char *json)
{
    if (!ws_open() || json == NULL) {
        return false;
    }
    int len = (int)strlen(json);
    int64_t started_us = esp_timer_get_time();
    int n = esp_websocket_client_send_text(s_ws, json, len,
                                           pdMS_TO_TICKS(QWEN_SEND_TIMEOUT_MS));
    long long elapsed_ms = (long long)((esp_timer_get_time() - started_us) / 1000);
    if (n != len) {
        ESP_LOGW(TAG, "WS send failed (%d/%d bytes, %lld ms)", n, len, elapsed_ms);
        return false;
    } else if (elapsed_ms > 500) {
        ESP_LOGW(TAG, "WS send delayed (%d bytes, %lld ms)", len, elapsed_ms);
    }
    return true;
}

static void send_hello(void)
{
    char id[16];
    snprintf(id, sizeof(id), "evt_%lu", (unsigned long)next_evt());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.hello");
    cJSON_AddStringToObject(root, "event_id", id);

    cJSON *proto = cJSON_AddObjectToObject(root, "protocol");
    cJSON_AddStringToObject(proto, "min", QWEN_PROTOCOL_VERSION);
    cJSON_AddStringToObject(proto, "max", QWEN_PROTOCOL_VERSION);

    cJSON *client = cJSON_AddObjectToObject(root, "client");
    cJSON_AddStringToObject(client, "type", "esp32");
    cJSON_AddStringToObject(client, "version", QWEN_CLIENT_VERSION);
    cJSON_AddStringToObject(client, "instance_id", s_instance_id);
    cJSON_AddStringToObject(client, "label", "BLU-Qwen Gateway");

    cJSON *caps = cJSON_AddArrayToObject(root, "capabilities");
    cJSON_AddItemToArray(caps, cJSON_CreateString("input.audio"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("playback.receipts"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("session.heartbeat"));
    cJSON_AddItemToArray(caps, cJSON_CreateString("session.takeover"));

    cJSON_AddStringToObject(root, "locale", "zh-CN");
    cJSON_AddStringToObject(root, "time_zone", "Asia/Shanghai");

    cJSON *conn = cJSON_AddObjectToObject(root, "connection");
    cJSON_AddBoolToObject(conn, "voice_enabled", true);
    cJSON_AddBoolToObject(conn, "input_enabled", true);
    cJSON_AddBoolToObject(conn, "output_enabled", true);
    cJSON_AddBoolToObject(conn, "text_only", false);
    cJSON_AddBoolToObject(conn, "takeover", true);

    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (printed) {
        send_json(printed);
        free(printed);
        ESP_LOGI(TAG, "Sent session.hello instance=%s", s_instance_id);
    }
}

static void send_pong(const char *request_event_id)
{
    if (!request_event_id || !request_event_id[0]) {
        return;
    }
    char json[QWEN_CONTROL_JSON_MAX];
    int len = snprintf(json, sizeof(json),
                       "{\"type\":\"session.pong\",\"event_id\":\"evt_%lu\",\"request_event_id\":\"%s\"}",
                       (unsigned long)next_evt(), request_event_id);
    if (len > 0 && (size_t)len < sizeof(json)) {
        send_json(json);
    }
}

static void handle_voice_ready(cJSON *root)
{
    cJSON *rate = cJSON_GetObjectItem(root, "inputSampleRate");
    if (cJSON_IsNumber(rate) && rate->valuedouble > 0) {
        s_input_rate = (int)rate->valuedouble;
    }
    set_state(QWEN_STATE_READY);
    ESP_LOGI(TAG, "voice.ready inputSampleRate=%d", s_input_rate);
    if (s_cb.on_ready) {
        s_cb.on_ready(s_input_rate);
    }
}

// Gateway audio fields are plain JSON strings; scan them without making a
// second large cJSON allocation in the ESP32's limited internal RAM.
static const char *json_string_field(const char *json, const char *key, size_t *length)
{
    char pattern[48];
    int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern)) {
        return NULL;
    }
    const char *field = strstr(json, pattern);
    if (field == NULL) {
        return NULL;
    }
    field += pattern_len;
    while (*field == ' ' || *field == '\t') {
        field++;
    }
    if (*field++ != ':') {
        return NULL;
    }
    while (*field == ' ' || *field == '\t') {
        field++;
    }
    if (*field++ != '"') {
        return NULL;
    }
    const char *end = strchr(field, '"');
    if (end == NULL) {
        return NULL;
    }
    *length = (size_t)(end - field);
    return field;
}

static bool response_discarded(const char *response_id)
{
    if (!response_id || !response_id[0]) return false;
    bool discarded = false;
    portENTER_CRITICAL(&s_discard_mux);
    for (unsigned i = 0; i < 4; i++) {
        if (strcmp(s_discarded_responses[i], response_id) == 0) {
            discarded = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_discard_mux);
    return discarded;
}

void qwen_client_discard_response(const char *response_id)
{
    if (!response_id || !response_id[0]) return;
    char id[QWEN_RESPONSE_ID_MAX] = {0};
    snprintf(id, sizeof(id), "%s", response_id);
    portENTER_CRITICAL(&s_discard_mux);
    memcpy(s_discarded_responses[s_discard_next], id, sizeof(id));
    s_discard_next = (s_discard_next + 1u) % 4u;
    portEXIT_CRITICAL(&s_discard_mux);
    __atomic_add_fetch(&s_audio_generation, 1, __ATOMIC_ACQ_REL);
}

static int base64_digit(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool decode_pcm_base64(const char *src, size_t length, unsigned char *dst,
                              size_t capacity, size_t *written)
{
    size_t out = 0;
    if ((length & 3u) != 0 || length / 4u * 3u > capacity) return false;
    for (size_t i = 0; i < length; i += 4) {
        int a = base64_digit((unsigned char)src[i]);
        int b = base64_digit((unsigned char)src[i + 1]);
        int c = src[i + 2] == '=' ? -2 : base64_digit((unsigned char)src[i + 2]);
        int d = src[i + 3] == '=' ? -2 : base64_digit((unsigned char)src[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 ||
            (c == -2 && d != -2) || ((c == -2 || d == -2) && i + 4 != length)) {
            return false;
        }
        dst[out++] = (unsigned char)((a << 2) | (b >> 4));
        if (c >= 0) {
            dst[out++] = (unsigned char)((b << 4) | (c >> 2));
            if (d >= 0) dst[out++] = (unsigned char)((c << 6) | d);
        }
    }
    *written = out;
    return true;
}

static void handle_audio_delta(const char *json, uint32_t generation)
{
    size_t b64_len = 0;
    const char *audio = json_string_field(json, "audio", &b64_len);
    if (audio == NULL || b64_len == 0 || (b64_len & 3u)) {
        ESP_LOGW(TAG, "audio.delta has invalid base64 payload");
        return;
    }

    int sample_rate = 24000;
    const char *rate = strstr(json, "\"sampleRate\":");
    if (rate != NULL) {
        int parsed = atoi(rate + strlen("\"sampleRate\":"));
        if (parsed > 0) {
            sample_rate = parsed;
        }
    }
    char response_id[QWEN_RESPONSE_ID_MAX] = {0};
    size_t response_len = 0;
    const char *response = json_string_field(json, "responseId", &response_len);
    if (response != NULL && response_len < sizeof(response_id)) {
        memcpy(response_id, response, response_len);
    }
    if (response_discarded(response_id)) return;
    if (response_id[0] && strcmp(response_id, s_last_audio_response_id) != 0) {
        ESP_LOGI(TAG, "First audio.delta response=%s bytes=%u queue=%u",
                 response_id, (unsigned)b64_len,
                 (unsigned)uxQueueMessagesWaiting(s_audio_queue));
        snprintf(s_last_audio_response_id, sizeof(s_last_audio_response_id),
                 "%s", response_id);
    }
    if (b64_len > QWEN_RX_JSON_INITIAL && !s_logged_large_audio) {
        ESP_LOGI(TAG, "Large audio.delta accepted: %u base64 bytes, %d Hz",
                 (unsigned)b64_len, sample_rate);
        s_logged_large_audio = true;
    }

    for (size_t offset = 0; offset < b64_len; offset += QWEN_RX_B64_CHARS) {
        if (generation != __atomic_load_n(&s_audio_generation, __ATOMIC_ACQUIRE) ||
            response_discarded(response_id)) {
            return;
        }
        size_t encoded = b64_len - offset;
        if (encoded > QWEN_RX_B64_CHARS) {
            encoded = QWEN_RX_B64_CHARS;
        }
        size_t decoded = 0;
        if (!decode_pcm_base64(audio + offset, encoded, (unsigned char *)s_rx_pcm,
                               QWEN_RX_PCM_MAX * sizeof(int16_t), &decoded) ||
            decoded == 0 || (decoded & 1u)) {
            ESP_LOGW(TAG, "audio.delta decode failed offset=%u", (unsigned)offset);
            return;
        }
        if (generation != __atomic_load_n(&s_audio_generation, __ATOMIC_ACQUIRE) ||
            response_discarded(response_id)) return;
        if (s_cb.on_audio_delta) {
            s_cb.on_audio_delta(s_rx_pcm, decoded / sizeof(int16_t),
                                sample_rate, response_id);
        }
        if ((offset / QWEN_RX_B64_CHARS & 7u) == 7u) {
            vTaskDelay(1);
        }
    }
}

static void audio_decode_task(void *arg)
{
    (void)arg;
    queued_audio_t item;
    for (;;) {
        if (xQueueReceive(s_audio_queue, &item, portMAX_DELAY) == pdTRUE) {
            if (item.generation == __atomic_load_n(&s_audio_generation, __ATOMIC_ACQUIRE)) {
                if (item.audio) {
                    handle_audio_delta(item.json, item.generation);
                } else if (s_cb.on_audio_done) {
                    size_t response_len = 0;
                    const char *response = json_string_field(item.json, "responseId",
                                                              &response_len);
                    char response_id[QWEN_RESPONSE_ID_MAX] = {0};
                    if (response && response_len < sizeof(response_id)) {
                        memcpy(response_id, response, response_len);
                    }
                    if (!response_discarded(response_id)) {
                        s_cb.on_audio_done(response_id);
                    }
                }
            }
            bool stale = item.generation != __atomic_load_n(&s_audio_generation,
                                                              __ATOMIC_ACQUIRE);
            free(item.json);
            if (!stale) vTaskDelay(1);
        }
    }
}

static void handle_message(const char *data, int len)
{
    if (data == NULL || len <= 0) {
        return;
    }

    size_t type_len = 0;
    const char *type_value = json_string_field(data, "type", &type_len);
    if (type_value != NULL &&
        ((type_len == strlen("audio.delta") &&
          memcmp(type_value, "audio.delta", type_len) == 0) ||
         (type_len == strlen("response.output_audio.delta") &&
          memcmp(type_value, "response.output_audio.delta", type_len) == 0))) {
        return;
    }
    if (len > QWEN_RX_JSON_INITIAL) {
        ESP_LOGW(TAG, "Ignoring oversized non-audio message (%d bytes)", len);
        return;
    }

    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed (%d bytes)", len);
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    const char *t = cJSON_IsString(type) ? type->valuestring : "";

    if (strcmp(t, "session.ready") == 0) {
        ESP_LOGI(TAG, "session.ready");
        set_state(QWEN_STATE_HANDSHAKE);
    } else if (strcmp(t, "session.ping") == 0) {
        cJSON *eid = cJSON_GetObjectItem(root, "event_id");
        if (cJSON_IsString(eid)) {
            send_pong(eid->valuestring);
        }
    } else if (strcmp(t, "voice.ready") == 0) {
        handle_voice_ready(root);
    } else if (strcmp(t, "voice.ownership") == 0) {
        cJSON *st = cJSON_GetObjectItem(root, "state");
        const char *own = cJSON_IsString(st) ? st->valuestring : "";
        s_voice_owned = (strcmp(own, "active") == 0);
        ESP_LOGI(TAG, "voice.ownership=%s", own);
    } else if (strcmp(t, "voice.state") == 0) {
        cJSON *state = cJSON_GetObjectItem(root, "state");
        cJSON *turn = cJSON_GetObjectItem(root, "turnId");
        ESP_LOGI(TAG, "voice.state=%s turn=%s",
                 cJSON_IsString(state) ? state->valuestring : "",
                 cJSON_IsString(turn) ? turn->valuestring : "");
    } else if (strcmp(t, "response.started") == 0) {
        cJSON *rid = cJSON_GetObjectItem(root, "responseId");
        ESP_LOGI(TAG, "response.started=%s",
                 cJSON_IsString(rid) ? rid->valuestring : "");
    } else if (strcmp(t, "input.suspend") == 0) {
        s_capture_ok = false;
        ESP_LOGI(TAG, "input.suspend - pausing microphone upload");
    } else if (strcmp(t, "input.resume") == 0) {
        s_capture_ok = true;
        ESP_LOGI(TAG, "input.resume - microphone upload on");
    } else if (strcmp(t, "response.interrupted") == 0) {
        __atomic_add_fetch(&s_audio_generation, 1, __ATOMIC_ACQ_REL);
        ESP_LOGI(TAG, "response.interrupted received");
        if (s_cb.on_playback_clear) {
            s_cb.on_playback_clear();
        }
    } else if (strcmp(t, "audio.done") == 0 ||
               strcmp(t, "response.output_audio.done") == 0) {
        cJSON *rid = cJSON_GetObjectItem(root, "responseId");
        if (!cJSON_IsString(rid)) {
            rid = cJSON_GetObjectItem(root, "response_id");
        }
        if (s_cb.on_audio_done) {
            s_cb.on_audio_done(cJSON_IsString(rid) ? rid->valuestring : "");
        }
    } else if (strcmp(t, "playback.clear") == 0) {
        __atomic_add_fetch(&s_audio_generation, 1, __ATOMIC_ACQ_REL);
        cJSON *reason = cJSON_GetObjectItem(root, "reason");
        ESP_LOGI(TAG, "playback.clear received reason=%s",
                 cJSON_IsString(reason) ? reason->valuestring : "");
        if (s_cb.on_playback_clear) {
            s_cb.on_playback_clear();
        }
    } else if (strcmp(t, "error") == 0) {
        cJSON *err = cJSON_GetObjectItem(root, "error");
        cJSON *msg = err ? cJSON_GetObjectItem(err, "message") : NULL;
        const char *text = cJSON_IsString(msg) ? msg->valuestring : "gateway error";
        ESP_LOGW(TAG, "Gateway error: %s", text);
        if (s_cb.on_error) {
            s_cb.on_error(text);
        }
    }

    cJSON_Delete(root);
}

static void handle_ws_data(const esp_websocket_event_data_t *ev)
{
    if (!ev || (ev->op_code != 0x01 && ev->op_code != 0x00) ||
        ev->data_len < 0 || ev->payload_len < 0 || ev->payload_offset < 0) {
        return;
    }
    if (ev->op_code == 0x01 && ev->payload_offset == 0) {
        s_rx_json_len = 0;
        s_rx_collecting = true;
        s_rx_discard = false;
    }
    if (!s_rx_collecting) {
        return;
    }
    if (ev->payload_offset == 0) {
        s_rx_frame_offset = 0;
    }
    if ((size_t)ev->payload_offset != s_rx_frame_offset ||
        ev->payload_offset > ev->payload_len ||
        (size_t)ev->data_len > (size_t)ev->payload_len - (size_t)ev->payload_offset) {
        s_rx_discard = true;
    }
    s_rx_frame_offset = (size_t)ev->payload_offset + (size_t)ev->data_len;
    if (!s_rx_discard && ev->data_len > 0) {
        size_t needed = s_rx_json_len + (size_t)ev->data_len;
        if (!ev->data_ptr || needed > QWEN_RX_JSON_MAX) {
            ESP_LOGW(TAG, "WebSocket message exceeds %u bytes", (unsigned)QWEN_RX_JSON_MAX);
            s_rx_discard = true;
        } else {
            if (needed > s_rx_json_capacity) {
                size_t capacity = s_rx_json_capacity;
                while (capacity < needed) {
                    capacity = capacity < QWEN_RX_JSON_MAX / 2u
                        ? capacity * 2u : QWEN_RX_JSON_MAX;
                }
                char *grown = heap_caps_realloc(s_rx_json, capacity + 1u,
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (grown == NULL) {
                    ESP_LOGW(TAG, "No PSRAM for %u-byte WebSocket message", (unsigned)capacity);
                    s_rx_discard = true;
                } else {
                    s_rx_json = grown;
                    s_rx_json_capacity = capacity;
                }
            }
            if (!s_rx_discard) {
                memcpy(s_rx_json + s_rx_json_len, ev->data_ptr, ev->data_len);
                s_rx_json_len = needed;
            }
        }
    }
    if (s_rx_frame_offset == (size_t)ev->payload_len && ev->fin) {
        if (!s_rx_discard && s_rx_json_len > 0) {
            s_rx_json[s_rx_json_len] = '\0';
            size_t type_len = 0;
            const char *type = json_string_field(s_rx_json, "type", &type_len);
            bool audio = type && type_len == strlen("audio.delta") &&
                         memcmp(type, "audio.delta", type_len) == 0;
            audio = audio || (type && type_len == strlen("response.output_audio.delta") &&
                             memcmp(type, "response.output_audio.delta", type_len) == 0);
            bool done = type && ((type_len == strlen("audio.done") &&
                         memcmp(type, "audio.done", type_len) == 0) ||
                         (type_len == strlen("response.output_audio.done") &&
                         memcmp(type, "response.output_audio.done", type_len) == 0));
            size_t response_len = 0;
            const char *response = (audio || done)
                ? json_string_field(s_rx_json, "responseId", &response_len) : NULL;
            char response_id[QWEN_RESPONSE_ID_MAX] = {0};
            if (response && response_len < sizeof(response_id)) {
                memcpy(response_id, response, response_len);
            }
            if ((audio || done) && response_discarded(response_id)) {
                s_rx_json_len = 0;
                s_rx_collecting = false;
                return;
            }
            if (audio || done) {
                queued_audio_t item = {
                    .json = s_rx_json,
                    .len = (int)s_rx_json_len,
                    .generation = __atomic_load_n(&s_audio_generation, __ATOMIC_ACQUIRE),
                    .audio = audio,
                };
                char *next = heap_caps_malloc(QWEN_RX_JSON_INITIAL + 1u,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (next && xQueueSend(s_audio_queue, &item, 0) == pdTRUE) {
                    s_rx_json = next;
                    s_rx_json_capacity = QWEN_RX_JSON_INITIAL;
                } else {
                    free(next);
                    ESP_LOGW(TAG, "Playback queue full; dropping %s (%d bytes)",
                             audio ? "audio.delta" : "audio.done", item.len);
                }
            } else {
                handle_message(s_rx_json, (int)s_rx_json_len);
            }
        }
        s_rx_json_len = 0;
        s_rx_collecting = false;
    }
}

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket connected");
        set_state(QWEN_STATE_HANDSHAKE);
        send_hello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket disconnected (close=%d error=%d errno=%d)",
                 ev ? ev->close_status_code : 0,
                 ev ? ev->error_handle.error_type : 0,
                 ev && ev->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT
                     ? ev->error_handle.esp_transport_sock_errno : 0);
        s_rx_json_len = 0;
        s_rx_collecting = false;
        set_state(QWEN_STATE_IDLE);
        if (s_cb.on_disconnected) {
            s_cb.on_disconnected(ev ? ev->close_status_code : 0);
        }
        break;
    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WebSocket closed (close=%d)",
                 ev ? ev->close_status_code : 0);
        break;
    case WEBSOCKET_EVENT_DATA:
        handle_ws_data(ev);
        break;
    case WEBSOCKET_EVENT_ERROR:
        if (ev && ev->error_handle.esp_ws_handshake_status_code == 401) {
            ESP_LOGE(TAG, "WebSocket rejected (HTTP 401): check device token and path");
        } else if (ev && ev->error_handle.esp_ws_handshake_status_code != 0) {
            ESP_LOGE(TAG, "WebSocket handshake failed (HTTP %d)",
                     ev->error_handle.esp_ws_handshake_status_code);
        } else {
            ESP_LOGE(TAG, "WebSocket error (type=%d errno=%d tls_err=0x%x cert_flags=0x%x)",
                     ev ? ev->error_handle.error_type : 0,
                     ev && ev->error_handle.error_type == WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT
                         ? ev->error_handle.esp_transport_sock_errno : 0,
                     ev ? ev->error_handle.esp_tls_last_esp_err : 0,
                     ev ? ev->error_handle.esp_tls_cert_verify_flags : 0);
        }
        break;
    default:
        break;
    }
}

esp_err_t qwen_client_init(const qwen_client_callbacks_t *callbacks)
{
    if (callbacks) {
        s_cb = *callbacks;
    } else {
        memset(&s_cb, 0, sizeof(s_cb));
    }

    if (s_tx_json == NULL) {
        s_tx_json = heap_caps_malloc(QWEN_TX_JSON_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_b64 == NULL) {
        s_b64 = heap_caps_malloc(QWEN_B64_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_rx_pcm == NULL) {
        s_rx_pcm = heap_caps_malloc(QWEN_RX_PCM_MAX * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_rx_json == NULL) {
        s_rx_json = heap_caps_malloc(QWEN_RX_JSON_INITIAL + 1u,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rx_json != NULL) {
            s_rx_json_capacity = QWEN_RX_JSON_INITIAL;
        }
    }
    if (!s_tx_json || !s_b64 || !s_rx_pcm || !s_rx_json) {
        ESP_LOGE(TAG, "Failed to allocate PSRAM buffers");
        return ESP_ERR_NO_MEM;
    }
    if (s_audio_queue == NULL) {
        s_audio_queue = xQueueCreate(QWEN_AUDIO_QUEUE_DEPTH, sizeof(queued_audio_t));
        if (s_audio_queue == NULL ||
            xTaskCreatePinnedToCore(audio_decode_task, "audio_decode", 4096,
                                    NULL, 8, NULL, 1) != pdPASS) {
            ESP_LOGE(TAG, "Failed to start audio decode task");
            return ESP_ERR_NO_MEM;
        }
    }
    s_rx_json_len = 0;
    s_rx_collecting = false;
    s_logged_large_audio = false;
    s_last_audio_response_id[0] = '\0';
    portENTER_CRITICAL(&s_discard_mux);
    memset(s_discarded_responses, 0, sizeof(s_discarded_responses));
    s_discard_next = 0;
    portEXIT_CRITICAL(&s_discard_mux);

    config_get_device_id(s_instance_id, sizeof(s_instance_id));
    if (s_instance_id[0] == '\0') {
        strncpy(s_instance_id, "esp32_qwen_gw", sizeof(s_instance_id) - 1);
    }
    set_state(QWEN_STATE_IDLE);
    ESP_LOGI(TAG, "Qwen client initialized, instance_id=%s", s_instance_id);
    return ESP_OK;
}

esp_err_t qwen_client_start(void)
{
    const system_config_t *cfg = config_get();
    if (!cfg || cfg->qwen_host[0] == '\0') {
        ESP_LOGE(TAG, "Qwen host is not configured");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_ws) {
        qwen_client_stop();
    }

    char uri[192];
    const char *scheme = cfg->qwen_tls ? "wss" : "ws";
    const char *path = cfg->qwen_path[0] ? cfg->qwen_path : "/api/realtime";
    // The Gateway defaults to sessionId=main; the device relay also accepts this path.
    int uri_len = snprintf(uri, sizeof(uri), "%s://%s:%u%s",
                           scheme, cfg->qwen_host, (unsigned)cfg->qwen_port, path);
    if (uri_len < 0 || (size_t)uri_len >= sizeof(uri)) {
        ESP_LOGE(TAG, "Qwen WebSocket URI is too long");
        return ESP_ERR_INVALID_ARG;
    }
    s_voice_owned = true;
    s_capture_ok = true;
    s_logged_large_audio = false;
    s_last_audio_response_id[0] = '\0';

    s_auth_header[0] = '\0';
    if (cfg->qwen_token[0]) {
        snprintf(s_auth_header, sizeof(s_auth_header),
                 "Authorization: Bearer %s\r\n", cfg->qwen_token);
    } else {
        ESP_LOGW(TAG, "Qwen device token is empty; protected Gateway or relay will reject the connection");
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = uri,
        .headers = cfg->qwen_token[0] ? s_auth_header : NULL,
        .crt_bundle_attach = cfg->qwen_tls ? esp_crt_bundle_attach : NULL,
        .skip_cert_common_name_check = false,
        .buffer_size = 8192,
        // Drain downlink frames ahead of capture sends; decoding and playback run elsewhere.
        .task_prio = 12,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
    };

    s_ws = esp_websocket_client_init(&ws_cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "esp_websocket_client_init failed");
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);

    set_state(QWEN_STATE_CONNECTING);
    ESP_LOGI(TAG, "Connecting to %s", uri);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        set_state(QWEN_STATE_IDLE);
    }
    return err;
}

void qwen_client_stop(void)
{
    __atomic_add_fetch(&s_audio_generation, 1, __ATOMIC_ACQ_REL);
    if (!s_ws) {
        set_state(QWEN_STATE_IDLE);
        return;
    }
    set_state(QWEN_STATE_CLOSING);
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    set_state(QWEN_STATE_IDLE);
    ESP_LOGI(TAG, "Qwen client stopped");
}

qwen_state_t qwen_client_get_state(void)
{
    return s_state;
}

bool qwen_client_is_ready(void)
{
    return s_state == QWEN_STATE_READY && ws_open();
}

int qwen_client_input_sample_rate(void)
{
    return s_input_rate > 0 ? s_input_rate : 16000;
}

esp_err_t qwen_client_send_pcm(const int16_t *pcm, size_t samples)
{
    if (!qwen_client_is_ready() || !s_voice_owned || !s_capture_ok ||
        pcm == NULL || samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples > QWEN_TX_PCM_MAX) {
        samples = QWEN_TX_PCM_MAX;
    }

    size_t olen = 0;
    int rc = mbedtls_base64_encode((unsigned char *)s_b64, QWEN_B64_MAX, &olen,
                                   (const unsigned char *)pcm,
                                   samples * sizeof(int16_t));
    if (rc != 0 || olen == 0 || olen >= QWEN_B64_MAX) {
        return ESP_FAIL;
    }
    s_b64[olen] = '\0';

    int n = snprintf(s_tx_json, QWEN_TX_JSON_MAX,
                     "{\"type\":\"input_audio_buffer.append\",\"event_id\":\"evt_%lu\",\"audio\":\"%s\"}",
                     (unsigned long)next_evt(), s_b64);
    if (n <= 0 || n >= QWEN_TX_JSON_MAX) {
        return ESP_ERR_NO_MEM;
    }
    int64_t started_us = esp_timer_get_time();
    bool sent = send_json(s_tx_json);
    uint32_t spent_ms = (uint32_t)((esp_timer_get_time() - started_us) / 1000);
    __atomic_add_fetch(&s_pcm_tx_spent_ms, spent_ms, __ATOMIC_RELAXED);
    uint32_t prev_max = __atomic_load_n(&s_pcm_tx_max_ms, __ATOMIC_RELAXED);
    while (spent_ms > prev_max &&
           !__atomic_compare_exchange_n(&s_pcm_tx_max_ms, &prev_max, spent_ms,
                                        false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    if (sent) {
        __atomic_add_fetch(&s_pcm_tx_frames, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&s_pcm_tx_samples, samples, __ATOMIC_RELAXED);
    } else {
        __atomic_add_fetch(&s_pcm_tx_failures, 1, __ATOMIC_RELAXED);
    }
    return sent ? ESP_OK : ESP_FAIL;
}

static void send_receipt(const char *type, const char *response_id)
{
    if (!qwen_client_is_ready() || !response_id || !response_id[0]) {
        return;
    }
    char json[QWEN_CONTROL_JSON_MAX];
    int len = snprintf(json, sizeof(json),
                       "{\"type\":\"%s\",\"event_id\":\"evt_%lu\",\"responseId\":\"%s\"}",
                       type, (unsigned long)next_evt(), response_id);
    if (len > 0 && (size_t)len < sizeof(json)) {
        int64_t started_us = esp_timer_get_time();
        bool sent = send_json(json);
        ESP_LOGI(TAG, "%s response=%s sent=%d elapsed=%lldms", type,
                 response_id, (int)sent,
                 (long long)((esp_timer_get_time() - started_us) / 1000));
    }
}

void qwen_client_send_playback_started(const char *response_id)
{
    send_receipt("playback.started", response_id);
}

void qwen_client_send_playback_ended(const char *response_id)
{
    send_receipt("playback.ended", response_id);
}

void qwen_client_send_playback_cancelled(const char *response_id)
{
    send_receipt("playback.cancelled", response_id);
}

void qwen_client_send_playback_cancelled_reason(const char *response_id,
                                               const char *reason)
{
    if (!qwen_client_is_ready() || !response_id || !response_id[0] ||
        !reason || !reason[0]) {
        return;
    }
    char json[QWEN_CONTROL_JSON_MAX];
    int len = snprintf(json, sizeof(json),
                       "{\"type\":\"playback.cancelled\",\"event_id\":\"evt_%lu\","
                       "\"responseId\":\"%s\",\"reason\":\"%s\"}",
                       (unsigned long)next_evt(), response_id, reason);
    if (len > 0 && (size_t)len < sizeof(json)) {
        int64_t started_us = esp_timer_get_time();
        bool sent = send_json(json);
        ESP_LOGI(TAG, "playback.cancelled response=%s reason=%s sent=%d elapsed=%lldms",
                 response_id, reason, (int)sent,
                 (long long)((esp_timer_get_time() - started_us) / 1000));
    }
}

void qwen_client_interrupt(void)
{
    if (!ws_open()) {
        return;
    }
    __atomic_add_fetch(&s_audio_generation, 1, __ATOMIC_ACQ_REL);
    char json[QWEN_CONTROL_JSON_MAX];
    snprintf(json, sizeof(json),
             "{\"type\":\"response.cancel\",\"event_id\":\"evt_%lu\"}",
             (unsigned long)next_evt());
    int64_t started_us = esp_timer_get_time();
    bool sent = send_json(json);
    ESP_LOGI(TAG, "response.cancel sent=%d elapsed=%lldms", (int)sent,
             (long long)((esp_timer_get_time() - started_us) / 1000));
}

void qwen_client_set_input_muted(bool muted)
{
    if (!ws_open()) {
        return;
    }
    char json[QWEN_CONTROL_JSON_MAX];
    snprintf(json, sizeof(json),
             "{\"type\":\"%s\",\"event_id\":\"evt_%lu\"}",
             muted ? "input.mute" : "input.unmute",
             (unsigned long)next_evt());
    send_json(json);
}
