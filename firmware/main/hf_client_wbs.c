/**
 * HFP Client has no public WBS toggle in ESP-IDF 6.
 * Bluedroid consults bta_hf_client_cb.msbc_enabled when sending AT+BAC
 * and when answering +BCS: TRUE → AT+BAC=1,2 (CVSD+mSBC), FALSE → AT+BAC=1.
 *
 * On WiFi we clear that flag so the phone is told we only support CVSD.
 */

#include "sdkconfig.h"
#include "esp_log.h"
#include <stdbool.h>

#if CONFIG_BT_HFP_CLIENT_ENABLE

#include "common/bt_target.h"
#include "bta_hf_client_int.h"

static const char *TAG = "hf_wbs";

void audio_coex_hf_client_prefer_msbc(bool enable)
{
#if (BTA_DYNAMIC_MEMORY == TRUE)
    if (bta_hf_client_cb_ptr == NULL) {
        return;
    }
#endif

    BOOLEAN want = enable ? TRUE : FALSE;
    BOOLEAN prev = bta_hf_client_cb.msbc_enabled;
    bta_hf_client_cb.msbc_enabled = want;

    if (prev != want) {
        ESP_LOGD(TAG, "HFP client BAC %s (msbc_enabled=%d)",
                 want ? "CVSD+mSBC" : "CVSD-only", (int)want);
        /* SLC already up: tell the AG the new codec list before the next SCO. */
        if (bta_hf_client_cb.scb.svc_conn) {
            bta_hf_client_send_at_bac();
        }
    }
}

#else /* !CONFIG_BT_HFP_CLIENT_ENABLE */

void audio_coex_hf_client_prefer_msbc(bool enable)
{
    (void)enable;
}

#endif
