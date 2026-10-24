#ifndef AUDIO_BRIDGE_H
#define AUDIO_BRIDGE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sco_codec_t sco_codec;
    int         input_rate;   /* Gateway voice.ready inputSampleRate */
    bool        capture;      /* false for A2DP speaker-only */
} audio_bridge_config_t;

esp_err_t audio_bridge_start(const audio_bridge_config_t *config);
esp_err_t audio_bridge_stop(void);
bool audio_bridge_is_active(void);

/** Bluetooth SCO capture → resample → Gateway input_audio_buffer.append */
void audio_bridge_bt_to_qwen(const uint8_t *pcm_data, uint32_t len);

/** Gateway audio.delta PCM → resample → SCO / A2DP playout */
void audio_bridge_qwen_to_bt(const int16_t *pcm, size_t samples, int sample_rate,
                             const char *response_id);

void audio_bridge_on_audio_done(const char *response_id);
void audio_bridge_clear_playback(void);
void audio_bridge_set_sco_codec(sco_codec_t sco_codec);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_BRIDGE_H */
