#ifndef WEB_CONFIG_H
#define WEB_CONFIG_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_CONFIG_RUNTIME_TIMEOUT_MS  (10 * 60 * 1000)

typedef enum {
    WEB_CONFIG_MODE_SETUP = 0,   /* first config: wait until applied */
    WEB_CONFIG_MODE_RUNTIME,     /* long-press while running: 10 min */
} web_config_mode_t;

typedef enum {
    WEB_CONFIG_APPLIED = 0,      /* new values saved; caller should reboot */
    WEB_CONFIG_TIMEOUT,          /* no submit within the window */
    WEB_CONFIG_UNAVAILABLE,      /* platform URL empty or HTTP failed */
    WEB_CONFIG_BUSY,             /* another session already running */
} web_config_result_t;

bool web_config_is_enabled(void);

/**
 * Register with the platform using the factory MAC device ID.
 * Idempotent: if the unit is already known, the existing device password
 * is returned and stored locally.
 */
esp_err_t web_config_register(void);

/**
 * POST current schema+values, poll until the user submits (or timeout).
 * On WEB_CONFIG_APPLIED the new parameters are already in NVS.
 *
 * @param timeout_ms  0 = wait indefinitely (setup); runtime uses 10 minutes
 */
web_config_result_t web_config_run_session(web_config_mode_t mode,
                                           uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* WEB_CONFIG_H */
