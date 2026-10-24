#ifndef AUDIO_COEX_H
#define AUDIO_COEX_H

#include <stdbool.h>
#include <stdint.h>
#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** True when WiFi is the active IP bearer (Ethernet not connected). */
bool audio_path_on_wifi(void);

/** Call while a media session is up so RF prefers Classic BT / SCO. */
void audio_coex_set_media(bool active);

/** True between audio_coex_set_media(true) and the matching false. */
bool audio_coex_media_active(void);

/**
 * Web/HTTP wants WiFi RF. Ignored while a call is using PREFER_BT.
 * busy=false restores BALANCE (unless media is still active).
 */
void audio_coex_set_http(bool busy);

/** Idle / SIP-keepalive: BALANCE. */
void audio_coex_set_idle(void);

/**
 * SCO output high-water mark in bytes. WiFi uses a deeper buffer so RTP
 * jitter is absorbed instead of trimming/underrunning.
 */
uint16_t audio_path_sco_hwm(sco_codec_t codec);

/** WiFi: prefer CVSD/NBS (less SCO airtime). Ethernet: allow mSBC. */
bool audio_path_prefer_narrowband(void);

/** Apply CVSD/mSBC preference before AT+BAC / SLC (HFP AG and HFP Client). */
void audio_coex_apply_wbs_preference(void);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_COEX_H */
