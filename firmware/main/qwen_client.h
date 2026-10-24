#ifndef QWEN_CLIENT_H
#define QWEN_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QWEN_STATE_IDLE = 0,
    QWEN_STATE_CONNECTING,
    QWEN_STATE_HANDSHAKE,
    QWEN_STATE_READY,
    QWEN_STATE_CLOSING,
} qwen_state_t;

typedef struct {
    void (*on_ready)(int input_sample_rate);
    void (*on_audio_delta)(const int16_t *pcm, size_t samples, int sample_rate,
                           const char *response_id);
    void (*on_audio_done)(const char *response_id);
    void (*on_playback_clear)(void);
    void (*on_disconnected)(int close_code);
    void (*on_error)(const char *message);
} qwen_client_callbacks_t;

esp_err_t qwen_client_init(const qwen_client_callbacks_t *callbacks);

/** Connect (or reconnect) to the configured Gateway / device relay. */
esp_err_t qwen_client_start(void);

/** Close the WebSocket and drop the session. */
void qwen_client_stop(void);

qwen_state_t qwen_client_get_state(void);
bool qwen_client_is_ready(void);
int qwen_client_input_sample_rate(void);

/**
 * Send mono PCM16LE. The caller must already resample to the rate from
 * voice.ready (typically 16000). The bridge sends 60 ms chunks.
 */
esp_err_t qwen_client_send_pcm(const int16_t *pcm, size_t samples);

typedef struct {
    uint32_t frames;
    uint32_t samples;
    uint32_t spent_ms;
    uint32_t max_ms;
    uint32_t failures;
} qwen_pcm_tx_stats_t;

/** Read and reset PCM send counters for the current reporting window. */
void qwen_client_take_pcm_tx_stats(qwen_pcm_tx_stats_t *stats);

void qwen_client_send_playback_started(const char *response_id);
void qwen_client_send_playback_ended(const char *response_id);
void qwen_client_send_playback_cancelled(const char *response_id);
void qwen_client_send_playback_cancelled_reason(const char *response_id,
                                               const char *reason);
void qwen_client_discard_response(const char *response_id);
void qwen_client_interrupt(void);
void qwen_client_set_input_muted(bool muted);

#ifdef __cplusplus
}
#endif

#endif /* QWEN_CLIENT_H */
