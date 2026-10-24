#ifndef PBAP_SYNC_H
#define PBAP_SYNC_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Register PBAP client after Bluedroid + HFP Client are up. */
esp_err_t pbap_sync_init(void);

/**
 * After HFP SLC is up, pull the phone book if whitelist is enabled.
 * The phone prompts the user to allow contact access.
 */
void pbap_sync_on_hfp_connected(void);

/** Drop PBAP if the phone disconnects. */
void pbap_sync_on_hfp_disconnected(void);

/**
 * Pause phone-book traffic during a call so ACL does not steal SCO slots.
 * When the call ends, a pending sync (if any) is not auto-restarted.
 */
void pbap_sync_set_call_active(bool active);

#ifdef __cplusplus
}
#endif

#endif /* PBAP_SYNC_H */
