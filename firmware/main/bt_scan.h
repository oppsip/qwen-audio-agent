#ifndef BT_SCAN_H
#define BT_SCAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_gap_bt_api.h"
#include "config_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_SCAN_MAX_DEVS  16
#define BT_SCAN_NAME_LEN  32
#define BT_SCAN_BDA_LEN   18

typedef enum {
    BT_SCAN_LINK_NONE = 0,
    BT_SCAN_LINK_CONNECTING,
    BT_SCAN_LINK_CONNECTED,
} bt_scan_link_t;

typedef struct {
    char    name[BT_SCAN_NAME_LEN];
    char    bda[BT_SCAN_BDA_LEN];
    int     rssi;
    bool    audio;
    bool    phone;
    bool    paired;
    uint8_t link;   /* bt_scan_link_t */
} bt_scan_dev_t;

/**
 * Feed a GAP inquiry result into the live list.
 * Always records named devices (for the config UI). Fills name_out when the
 * EIR / BDNAME is present. Safe to call from the GAP callback.
 */
bool bt_scan_on_disc_res(const esp_bt_gap_cb_param_t *param,
                         char *name_out, size_t name_len);

void bt_scan_on_disc_state(const esp_bt_gap_cb_param_t *param);

/** Start a Classic inquiry if one is not already running. */
void bt_scan_start(void);

uint32_t bt_scan_generation(void);

bool bt_scan_is_discovering(void);

/** Copy up to max devices. Returns the number copied. */
int bt_scan_copy(bt_scan_dev_t *out, int max);

/** Merge the controller bond list. Call from the web-config task. */
void bt_scan_refresh_bonded(void);

void bt_scan_on_auth_cmpl(const uint8_t *bda, const char *name);
void bt_scan_on_remote_name(const uint8_t *bda, const char *name);

void bt_scan_set_link(const uint8_t *bda, bt_scan_link_t link);

/**
 * Bring up Classic BT (controller + Bluedroid + GAP) if it is not already
 * running, so first-boot web config can inquire nearby devices.
 * No-op when HFP / A2DP / AG already own the stack.
 */
esp_err_t bt_scan_ensure_stack(void);

/** Tear down a stack started by bt_scan_ensure_stack(). No-op otherwise. */
void bt_scan_release_stack(void);

/** Apply COD / inquiry filter for the selected audio mode. Starts a scan
 *  only if none is already running (does not cancel HFP/A2DP inquiry). */
void bt_scan_set_role(bt_audio_mode_t mode);

bt_audio_mode_t bt_scan_get_role(void);

/** Inquiry length in 1.28 s units. Shorter when WiFi is the uplink. */
uint8_t bt_scan_inquiry_len(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_SCAN_H */
