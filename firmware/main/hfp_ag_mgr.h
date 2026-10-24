#ifndef HFP_AG_MGR_H
#define HFP_AG_MGR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "audio_codec.h"
#include "hfp_client_mgr.h"  /* hfp_conn_state_t */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * HFP Audio Gateway callbacks.
 *
 * ESP32 is the AG (phone/gateway); a Bluetooth headset is the HF unit.
 * Call control events come FROM the headset (ATA / CHUP / ATD).
 */
typedef struct {
    void (*on_connection_state)(hfp_conn_state_t state);
    void (*on_hf_answer)(void);                  /* headset pressed answer (ATA) */
    void (*on_hf_hangup)(void);                  /* headset pressed hangup (CHUP) */
    void (*on_hf_dial)(const char *number);      /* ATD / BLDN; number may be empty */
    void (*on_audio_data)(const uint8_t *data, uint32_t len);
    void (*on_audio_codec_changed)(sco_codec_t codec);
} hfp_ag_callbacks_t;

/**
 * Initialize HFP Audio Gateway. Device is discoverable as a phone so
 * headsets can pair. If target_headset_name is non-empty, scan and connect
 * to that headset; empty = wait for the headset to connect (or last peer).
 */
esp_err_t hfp_ag_init(const char *device_name, const char *target_headset_name,
                      const hfp_ag_callbacks_t *callbacks);

/** Notify the headset of an incoming SIP call (unsolicited RING + CLIP).
 *  The headset plays its own ringtone; ATA answers, CHUP rejects/hangs up. */
esp_err_t hfp_ag_notify_incoming(const char *number);

/** Confirm the incoming call was answered (indicators + SCO). */
esp_err_t hfp_ag_answer(void);

/** Reject a ringing incoming call. */
esp_err_t hfp_ag_reject(void);

/** End the current call (active or setup) and release SCO. */
esp_err_t hfp_ag_end_call(void);

/** Notify the headset that AG originated an outgoing call. */
esp_err_t hfp_ag_notify_outgoing(const char *number);

/** Update indicators to alerting (remote ringing). */
esp_err_t hfp_ag_notify_outgoing_alerting(void);

/** Mark the call active and open SCO (after SIP 200 OK). */
esp_err_t hfp_ag_notify_call_active(void);

/** Push PCM into the SCO TX ring (RTP → headset speaker). */
void hfp_ag_push_audio(const uint8_t *data, uint32_t len);

hfp_conn_state_t hfp_ag_get_conn_state(void);
sco_codec_t hfp_ag_get_sco_codec(void);

/** Playout depth currently buffered in the SCO TX ring, ms (-1 = no SCO). */
int hfp_ag_sco_out_level_ms(void);
void hfp_ag_sco_tx_stats(uint32_t *calls, uint32_t *pad, uint32_t *trims);
void hfp_ag_set_auto_reconnect(bool enable);

#ifdef __cplusplus
}
#endif

#endif /* HFP_AG_MGR_H */
