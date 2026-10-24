#ifndef GATEWAY_CONTROLLER_H
#define GATEWAY_CONTROLLER_H

#include "esp_err.h"
#include "hfp_client_mgr.h"
#include "hfp_ag_mgr.h"
#include "a2dp_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CALL_STATE_IDLE = 0,
    CALL_STATE_CONNECTING,
    CALL_STATE_ACTIVE,
} call_state_t;

esp_err_t gateway_init(void);

/** Start a Qwen session if Bluetooth is ready and we are idle. */
void gateway_try_start_session(const char *reason);

const hfp_callbacks_t *gateway_get_hfp_callbacks(void);
const a2dp_callbacks_t *gateway_get_a2dp_callbacks(void);
const hfp_ag_callbacks_t *gateway_get_hfp_ag_callbacks(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_CONTROLLER_H */
